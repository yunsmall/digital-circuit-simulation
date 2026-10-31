// VirtioMmio：modern virtio-mmio 寄存器、队列握手、blk 读写
#include <gtest/gtest.h>

#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/memory/ROM.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/VirtioMmio.h>

using namespace dsc;

namespace {

struct VirtioFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    VirtioMmio *dev = nullptr;
    WishboneMemory *ram = nullptr;
    ROM *rom = nullptr;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *ack, *irq;
    Net *m_adr, *m_dat_o, *m_we, *m_sel, *m_stb, *m_cyc, *m_dat_i, *m_ack;

    explicit VirtioFixture(bool with_rom, VirtioMmio::Kind kind = VirtioMmio::Kind::Blk) {
        auto d = std::make_unique<VirtioMmio>("vd0", kind, "");
        dev = d.get();
        c->addComponent(std::move(d));
        auto r = std::make_unique<WishboneMemory>("ram0", 12, 32); // 4 KB
        ram = r.get();
        c->addComponent(std::move(r));
        if (with_rom) {
            auto o = std::make_unique<ROM>("rom0", 9, 8); // 512 字节
            rom = o.get();
            c->addComponent(std::move(o));
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
        c->setWire(clk->id(), {0});
        c->setWire(stb->id(), {0});
        c->setWire(cyc->id(), {0});
        c->setWire(we->id(), {0});
        for (int i = 0; i < 2; i++) tick();
    }

    void tick() {
        c->setWire(clk->id(), {0});
        c->tick();
        c->setWire(clk->id(), {1});
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
        c->setWire(we->id(), {1});
        c->setWire(sel->id(), {0x0F});
        c->setWire(stb->id(), {1});
        c->setWire(cyc->id(), {1});
        for (int i = 0; i < 4; i++) tick();
        c->setWire(stb->id(), {0});
        c->setWire(cyc->id(), {0});
        for (int i = 0; i < 2; i++) tick();
    }
    uint32_t read(uint32_t a) {
        set32(adr, a);
        c->setWire(we->id(), {0});
        c->setWire(sel->id(), {0x0F});
        c->setWire(stb->id(), {1});
        c->setWire(cyc->id(), {1});
        for (int i = 0; i < 4; i++) tick();
        auto v = get32(dat_o);
        c->setWire(stb->id(), {0});
        c->setWire(cyc->id(), {0});
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

    // 一个 blk 请求：desc0=请求头，desc1=数据，desc2=状态
    void setupQueue(uint32_t desc, uint32_t avail, uint32_t used, uint16_t desc_idx,
                    uint32_t hdr_off, uint32_t data_off, uint32_t status_off, uint32_t data_len,
                    uint8_t type, uint64_t sector, bool write_data) {
        uint8_t *m = mem();
        // desc0: addr/len/flags/next
        const uint16_t flags0 = 1; // NEXT
        const uint16_t flags1 = (uint16_t) ((write_data ? 2 : 0) | 1);
        const uint16_t flags2 = write_data ? 2 : 0;
        struct D {
            uint64_t addr;
            uint32_t len;
            uint16_t flags;
            uint16_t next;
        } d0{hdr_off, 16, flags0, (uint16_t) (desc_idx + 1)},
                d1{data_off, data_len, flags1, (uint16_t) (desc_idx + 2)},
                d2{status_off, 1, flags2, 0};
        std::memcpy(m + desc + desc_idx * 16, &d0, 16);
        std::memcpy(m + desc + (desc_idx + 1) * 16, &d1, 16);
        std::memcpy(m + desc + (desc_idx + 2) * 16, &d2, 16);
        // 请求头
        std::memset(m + hdr_off, 0, 16);
        std::memcpy(m + hdr_off, &type, 4);
        std::memcpy(m + hdr_off + 8, &sector, 8);
        // avail
        put16(avail + 0, 0);
        put16(avail + 2, 1);
        put16(avail + 4, desc_idx);
        // used
        put16(used + 0, 0);
        put16(used + 2, 0);
    }

    void configureQueue(uint32_t desc, uint32_t avail, uint32_t used, uint32_t num = 4) {
        write(0x030, 0); // queue_sel
        write(0x038, num); // queue_num
        write(0x080, desc);
        write(0x084, 0);
        write(0x090, avail);
        write(0x094, 0);
        write(0x0A0, used);
        write(0x0A4, 0);
        write(0x044, 1); // ready
    }
};

} // namespace

TEST(VirtioTest, MagicVersionAndDeviceId) {
    VirtioFixture f(false);
    EXPECT_EQ(f.read(0x000), 0x74726976u) << "magic 应为 \"virt\"";
    EXPECT_EQ(f.read(0x004), 2u) << "只实现 modern（version 2）";
    EXPECT_EQ(f.read(0x008), 2u) << "blk 的 device id 是 2";
    EXPECT_EQ(f.read(0x00C), 0x444353u);
}

TEST(VirtioTest, FeatureSelectors) {
    VirtioFixture f(false);
    f.write(0x014, 0); // sel 0
    EXPECT_EQ(f.read(0x010), 0u) << "没有额外特性";
    f.write(0x014, 1); // sel 1
    EXPECT_EQ(f.read(0x010), 1u) << "sel1 必须声明 VIRTIO_F_VERSION_1";
}

TEST(VirtioTest, ReadFromRomImage) {
    VirtioFixture f(true);
    uint8_t *rp = f.rom->writableMemPtr();
    ASSERT_NE(rp, nullptr);
    for (int i = 0; i < 512; i++)
        rp[i] = (uint8_t) i;

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0); // notify
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "请求完成后 used.idx 应加一";
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) i) << "第 " << i << " 字节不匹配";
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 1) << "完成请求后应拉中断";
}

TEST(VirtioTest, WriteToReadOnlyRomReturnsRoError) {
    VirtioFixture f(true);
    uint8_t *rp = f.rom->writableMemPtr();
    for (int i = 0; i < 512; i++)
        rp[i] = 0;
    EXPECT_NE(f.read(0x010) & (1u << 5), 0u) << "只读后端应声明 VIRTIO_BLK_F_RO";

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    std::memset(f.mem() + data, 0xAA, 16);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 3) << "对只读后端写入应返回 RO 错误";
    EXPECT_EQ(rp[0], 0) << "镜像不能被改动";
}

TEST(VirtioTest, UnsupportedRequestReturnsUnsupp) {
    VirtioFixture f(true);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupQueue(desc, avail, used, 0, hdr, data, status, 16, 99, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.mem()[status], 2) << "未知请求类型应返回 UNSUPP";
}

TEST(VirtioTest, InterruptAckClearsLine) {
    VirtioFixture f(true);
    for (int i = 0; i < 512; i++)
        f.rom->writableMemPtr()[i] = 0;
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 16, 0, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.read(0x060) & 1u, 1u) << "interrupt_status 应置 VRING 位";
    f.write(0x064, 1); // ack
    for (int i = 0; i < 2; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 0) << "ack 后中断线应撤销";
}

TEST(VirtioTest, StatusZeroResetsQueue) {
    VirtioFixture f(false);
    f.write(0x030, 0);
    f.write(0x038, 4);
    f.write(0x044, 1);
    f.write(0x070, 0);
    f.write(0x030, 0);
    EXPECT_EQ(f.read(0x044), 0u) << "status 写 0 应复位队列";
}

// virtio-rng：descriptor 是设备可写的数据段，随机数在 JIT 里生成
TEST(VirtioRng, FillsBuffer) {
    VirtioFixture f(false, VirtioMmio::Kind::Rng);
    const uint32_t desc = 0x100, avail = 0x200, used = 0x300, data = 0x400;

    struct D {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } d{data, 8, 2, 0}; // F_WRITE
    std::memcpy(f.mem() + desc, &d, 16);
    f.put16(avail + 0, 0);
    f.put16(avail + 2, 1);
    f.put16(avail + 4, 0);
    f.put16(used + 0, 0);
    std::memset(f.mem() + data, 0, 8);

    f.write(0x030, 0);    // queue_sel
    f.write(0x038, 8);    // queue_num
    f.write(0x080, desc); // desc 地址
    f.write(0x090, avail);
    f.write(0x0A0, used);
    f.write(0x044, 1);    // queue_ready
    f.write(0x070, 1);    // status
    f.write(0x050, 0);    // notify

    for (int i = 0; i < 400; i++)
        f.tick();

    uint64_t v = 0;
    std::memcpy(&v, f.mem() + data, 8);
    std::fprintf(stderr, "desc: ");
    for (int i = 0; i < 16; i++) std::fprintf(stderr, "%02X ", f.mem()[desc + i]);
    std::fprintf(stderr, "used: ");
    for (int i = 0; i < 16; i++) std::fprintf(stderr, "%02X ", f.mem()[used + i]);
    std::fprintf(stderr, "data=%llX\n", (unsigned long long) v);
    EXPECT_NE(v, 0u);
    EXPECT_EQ(f.get16(used + 2), 1); // used.idx（vring_used 的 +0 是 flags）
    EXPECT_EQ(f.get16(used + 4), 0); // used.ring[0].id
    uint32_t used_len = 0;
    std::memcpy(&used_len, f.mem() + used + 8, 4);
    EXPECT_EQ(used_len, 8u);
}

namespace {

// rng 队列搭建：desc_in 处放 desc_num 个描述符，avail 环里放 desc_num 个请求
struct RngSetup {
    uint32_t desc, avail, used, data;
};

void rngConfigure(VirtioFixture &f, const RngSetup &s, uint16_t desc_head, uint16_t avail_idx) {
    f.put16(s.avail + 0, 0);        // flags
    f.put16(s.avail + 2, avail_idx); // idx
    f.put16(s.used + 0, 0);
    f.put16(s.used + 2, 0);
    f.write(0x030, 0);
    f.write(0x038, 8);
    f.write(0x080, s.desc);
    f.write(0x090, s.avail);
    f.write(0x0A0, s.used);
    f.write(0x044, 1);
    f.write(0x070, 1);
}

void putDesc(VirtioFixture &f, uint32_t desc_base, uint16_t idx, uint64_t addr, uint32_t len,
             uint16_t flags, uint16_t next) {
    struct D {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } d{addr, len, flags, next};
    std::memcpy(f.mem() + desc_base + idx * 16, &d, 16);
}

uint32_t usedLen(VirtioFixture &f, const RngSetup &s, int i) {
    uint32_t v = 0;
    std::memcpy(&v, f.mem() + s.used + 4 + 8 * i + 4, 4);
    return v;
}

} // namespace

// 大缓冲区：256 字节要跨 4 轮 64 字节的主端口事务
TEST(VirtioRng, LargeBuffer) {
    VirtioFixture f(false, VirtioMmio::Kind::Rng);
    RngSetup s{0x100, 0x200, 0x300, 0x400};
    putDesc(f, s.desc, 0, s.data, 256, 2, 0);
    std::memset(f.mem() + s.data, 0, 256);
    f.put16(s.avail + 4, 0);
    rngConfigure(f, s, 0, 1);
    f.write(0x050, 0);

    for (int i = 0; i < 1500; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(usedLen(f, s, 0), 256u);
    // 高 64 字节也要被写到，不能只写第一块
    uint64_t tail = 0;
    std::memcpy(&tail, f.mem() + s.data + 248, 8);
    EXPECT_NE(tail, 0u);
}

// 链式描述符：两个可写段，written 是两段之和
TEST(VirtioRng, ChainedDescriptors) {
    VirtioFixture f(false, VirtioMmio::Kind::Rng);
    RngSetup s{0x100, 0x200, 0x300, 0x400};
    putDesc(f, s.desc, 0, s.data, 8, 3, 1);        // NEXT|WRITE
    putDesc(f, s.desc, 1, s.data + 8, 8, 2, 0);    // WRITE
    f.put16(s.avail + 4, 0);
    rngConfigure(f, s, 0, 1);
    f.write(0x050, 0);

    for (int i = 0; i < 800; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(usedLen(f, s, 0), 16u);
}

// 队列没 ready 时不处理请求
TEST(VirtioRng, IgnoresWhenNotReady) {
    VirtioFixture f(false, VirtioMmio::Kind::Rng);
    RngSetup s{0x100, 0x200, 0x300, 0x400};
    putDesc(f, s.desc, 0, s.data, 8, 2, 0);
    f.put16(s.avail + 4, 0);
    // 不写 queue_ready
    f.write(0x030, 0);
    f.write(0x038, 8);
    f.write(0x080, s.desc);
    f.write(0x090, s.avail);
    f.write(0x0A0, s.used);
    f.write(0x070, 1);
    f.write(0x050, 0);

    for (int i = 0; i < 400; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 0u);
}

// 连续两个请求：used.idx 递增到 2，两个 used 条目各自记录长度
TEST(VirtioRng, TwoRequests) {
    VirtioFixture f(false, VirtioMmio::Kind::Rng);
    RngSetup s{0x100, 0x200, 0x300, 0x400};
    putDesc(f, s.desc, 0, s.data, 4, 2, 0);
    putDesc(f, s.desc, 1, s.data + 16, 4, 2, 0);
    f.put16(s.avail + 4, 0);
    f.put16(s.avail + 6, 1);
    rngConfigure(f, s, 0, 2);
    f.write(0x050, 0);

    for (int i = 0; i < 1200; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 2);
    EXPECT_EQ(usedLen(f, s, 0), 4u);
    EXPECT_EQ(usedLen(f, s, 1), 4u);
}
