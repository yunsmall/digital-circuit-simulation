// virtio 测试共用的电路夹具：把设备接上 Wishbone 从端口和一块 guest RAM。
#pragma once
#include <gtest/gtest.h>

#include <cstring>
#include <type_traits>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/VirtioBlk.h>

namespace dsc::test {

// blk 的介质来源。ReadOnly 用来验证写请求返回 RO 错误
enum class BlkImage { None, ReadWrite, ReadOnly };

template <typename Dev>
struct VirtioFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Dev *dev = nullptr;
    WishboneMemory *ram = nullptr;
    std::vector<uint8_t> image;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *ack, *irq;
    Net *m_adr, *m_dat_o, *m_we, *m_sel, *m_stb, *m_cyc, *m_dat_i, *m_ack;

    static uint32_t imageRead(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
        auto *v = static_cast<std::vector<uint8_t> *>(ctx);
        if (off + len > v->size())
            return 0;
        std::memcpy(buf, v->data() + (size_t) off, len);
        return 1;
    }
    static uint32_t imageWrite(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
        auto *v = static_cast<std::vector<uint8_t> *>(ctx);
        if (off + len > v->size())
            return 0;
        std::memcpy(v->data() + (size_t) off, buf, len);
        return 1;
    }

    // custom 非空时优先于 img：需要观察设备交给后端的 64 位偏移时用
    explicit VirtioFixture(BlkImage img = BlkImage::None, const VirtioBlk::BackendOps *custom = nullptr) {
        auto d = std::make_unique<Dev>("vd0");
        dev = d.get();
        c->addComponent(std::move(d));
        auto r = std::make_unique<WishboneMemory>("ram0", 12, 32); // 4 KB
        ram = r.get();
        c->addComponent(std::move(r));
        if constexpr (std::is_same_v<Dev, VirtioBlk>) {
            if (custom) {
                dev->setBackend(*custom);
            }
            else if (img != BlkImage::None) {
                image.assign(512, 0);
                VirtioBlk::BackendOps ops;
                ops.ctx = &image;
                ops.read = &VirtioFixture::imageRead;
                ops.write = img == BlkImage::ReadWrite ? &VirtioFixture::imageWrite : nullptr;
                ops.size = image.size();
                dev->setBackend(ops);
            }
        }
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat_i = c->createNet("di");
        dat_o = c->createNet("do"); we = c->createNet("we"); sel = c->createNet("sel");
        stb = c->createNet("stb"); cyc = c->createNet("cyc"); ack = c->createNet("ack");
        irq = c->createNet("irq");
        for (auto [pin, net]: {std::pair{"clk", clk}, {"wb_adr_i", adr}, {"wb_dat_i", dat_i},
                               {"wb_we_i", we}, {"wb_sel_i", sel}, {"wb_stb_i", stb}, {"wb_cyc_i", cyc},
                               {"wb_dat_o", dat_o}, {"wb_ack_o", ack}, {"irq", irq}})
            c->connect(dev, pin, net);
        // 主端口接存储器的从端口：设备通过总线访问 virtqueue，不直接碰内存
        m_adr = c->createNet("m_adr");
        m_dat_o = c->createNet("m_dat_o");
        m_we = c->createNet("m_we");
        m_sel = c->createNet("m_sel");
        m_stb = c->createNet("m_stb");
        m_cyc = c->createNet("m_cyc");
        m_dat_i = c->createNet("m_dat_i");
        m_ack = c->createNet("m_ack");
        for (auto [pin, net]: {std::pair{"m_adr_o", m_adr}, {"m_dat_o", m_dat_o}, {"m_we_o", m_we},
                               {"m_sel_o", m_sel}, {"m_stb_o", m_stb}, {"m_cyc_o", m_cyc},
                               {"m_dat_i", m_dat_i}, {"m_ack_i", m_ack}})
            c->connect(dev, pin, net);
        c->connect(ram, "wb_adr_i", m_adr);
        c->connect(ram, "wb_dat_i", m_dat_o);
        c->connect(ram, "wb_we_i", m_we);
        c->connect(ram, "wb_sel_i", m_sel);
        c->connect(ram, "wb_stb_i", m_stb);
        c->connect(ram, "wb_cyc_i", m_cyc);
        c->connect(ram, "wb_dat_o", m_dat_i);
        c->connect(ram, "wb_ack_o", m_ack);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
        c->setWire(clk->id(), 0);
        c->setWire(stb->id(), 0);
        c->setWire(cyc->id(), 0);
        c->setWire(we->id(), 0);
        for (int i = 0; i < 2; i++) tick();
    }

    void tick() {
        c->setWire(clk->id(), 0);
        c->tick();
        c->setWire(clk->id(), 1);
        c->tick();
    }
    void set32(Net *n, uint32_t v) {
        c->setWire(n->id(), {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)});
    }
    uint32_t get32(Net *n) {
        auto v = c->getWire(n->id());
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    void write(uint32_t a, uint32_t v) {
        set32(adr, a); set32(dat_i, v);
        c->setWire(we->id(), 1);
        c->setWire(sel->id(), 0x0F);
        c->setWire(stb->id(), 1);
        c->setWire(cyc->id(), 1);
        for (int i = 0; i < 4; i++) tick();
        c->setWire(stb->id(), 0);
        c->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; i++) tick();
    }
    uint32_t read(uint32_t a) {
        set32(adr, a);
        c->setWire(we->id(), 0);
        c->setWire(sel->id(), 0x0F);
        c->setWire(stb->id(), 1);
        c->setWire(cyc->id(), 1);
        for (int i = 0; i < 4; i++) tick();
        auto v = get32(dat_o);
        c->setWire(stb->id(), 0);
        c->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; i++) tick();
        return v;
    }
    uint8_t *mem() { return ram->writableMemPtr(); }
    void put32(uint32_t off, uint32_t v) { std::memcpy(mem() + off, &v, 4); }
    void put16(uint32_t off, uint16_t v) { std::memcpy(mem() + off, &v, 2); }
    uint16_t get16(uint32_t off) {
        uint16_t v;
        std::memcpy(&v, mem() + off, 2);
        return v;
    }
    void putDesc(uint32_t base, uint16_t idx, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
        struct D {
            uint64_t addr;
            uint32_t len;
            uint16_t flags;
            uint16_t next;
        } d{addr, len, flags, next};
        std::memcpy(mem() + base + idx * 16, &d, 16);
    }
    uint32_t usedLen(uint32_t used, int i) {
        uint32_t v = 0;
        std::memcpy(&v, mem() + used + 4 + 8 * i + 4, 4);
        return v;
    }

    // 一个 blk 请求：desc0=请求头，desc1=数据，desc2=状态
    void setupBlkQueue(uint32_t desc, uint32_t avail, uint32_t used, uint16_t desc_idx,
                       uint32_t hdr_off, uint32_t data_off, uint32_t status_off, uint32_t data_len,
                       uint8_t type, uint64_t sector, bool write_data) {
        uint8_t *m = mem();
        const uint16_t flags0 = 1; // NEXT
        const uint16_t flags1 = (uint16_t) ((write_data ? 2 : 0) | 1);
        const uint16_t flags2 = write_data ? 2 : 0;
        putDesc(desc, desc_idx, hdr_off, 16, flags0, (uint16_t) (desc_idx + 1));
        putDesc(desc, (uint16_t) (desc_idx + 1), data_off, data_len, flags1, (uint16_t) (desc_idx + 2));
        putDesc(desc, (uint16_t) (desc_idx + 2), status_off, 1, flags2, 0);
        std::memset(m + hdr_off, 0, 16);
        // type 是 uint8_t 参数，不能直接 memcpy 4 字节：那会读栈上相邻的垃圾，
        // 不同编译器下头字段就变成随机值（GCC 下设备会把它当成非法请求类型）
        const uint32_t type32 = type;
        std::memcpy(m + hdr_off, &type32, 4);
        std::memcpy(m + hdr_off + 8, &sector, 8);
        put16(avail + 0, 0);
        put16(avail + 2, 1);
        put16(avail + 4, desc_idx);
        put16(used + 0, 0);
        put16(used + 2, 0);
    }

    // q 是队列号：blk/rng/input 用 0，net 的收包/发包分别是 0 和 1
    void configureQueue(uint32_t desc, uint32_t avail, uint32_t used, uint32_t num = 4, uint32_t q = 0) {
        write(0x030, q);
        write(0x038, num);
        write(0x080, desc);
        write(0x084, 0);
        write(0x090, avail);
        write(0x094, 0);
        write(0x0A0, used);
        write(0x0A4, 0);
        write(0x044, 1);
    }
};

struct QueueAddrs {
    uint32_t desc, avail, used, data;
};

template <typename Dev>
void prepareQueue(VirtioFixture<Dev> &f, const QueueAddrs &s, uint16_t n) {
    f.put16(s.avail + 0, 0);
    f.put16(s.avail + 2, n);
    f.put16(s.used + 0, 0);
    f.put16(s.used + 2, 0);
}

} // namespace dsc::test
