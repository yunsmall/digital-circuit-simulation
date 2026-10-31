// virtio 设备：modern virtio-mmio 寄存器、队列握手、各设备的数据搬运
#include <gtest/gtest.h>

#include <cstring>
#include <type_traits>
#include <vector>
#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/VirtioBlk.h>
#include <dcs/components/platform/VirtioInput.h>
#include <dcs/components/platform/VirtioNet.h>
#include <dcs/components/platform/VirtioRng.h>

using namespace dsc;

namespace {

// blk 的介质来源。Rob 是只读后端，用来验证写请求返回 RO 错误
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

    explicit VirtioFixture(BlkImage img = BlkImage::None) {
        auto d = std::make_unique<Dev>("vd0");
        dev = d.get();
        c->addComponent(std::move(d));
        auto r = std::make_unique<WishboneMemory>("ram0", 12, 32); // 4 KB
        ram = r.get();
        c->addComponent(std::move(r));
        if constexpr (std::is_same_v<Dev, VirtioBlk>) {
            if (img != BlkImage::None) {
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
        std::memcpy(m + hdr_off, &type, 4);
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

} // namespace

TEST(VirtioTest, MagicVersionAndDeviceId) {
    VirtioFixture<VirtioBlk> f;
    EXPECT_EQ(f.read(0x000), 0x74726976u) << "magic 应为 \"virt\"";
    EXPECT_EQ(f.read(0x004), 2u) << "只实现 modern（version 2）";
    EXPECT_EQ(f.read(0x008), 2u) << "blk 的 device id 是 2";
    EXPECT_EQ(f.read(0x00C), 0x444353u);
}

TEST(VirtioTest, FeatureSelectors) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x014, 0); // sel 0
    EXPECT_EQ(f.read(0x010), 0u) << "没有额外特性";
    f.write(0x014, 1); // sel 1
    EXPECT_EQ(f.read(0x010), 1u) << "sel1 必须声明 VIRTIO_F_VERSION_1";
}

TEST(VirtioTest, ReadFromBackendImage) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    for (int i = 0; i < 512; i++)
        f.image[i] = (uint8_t) i;

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0); // notify
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "请求完成后 used.idx 应加一";
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) i) << "第 " << i << " 字节不匹配";
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 1) << "完成请求后应拉中断";
}

TEST(VirtioTest, WriteToReadOnlyImageReturnsRoError) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadOnly);
    EXPECT_NE(f.read(0x010) & (1u << 5), 0u) << "只读后端应声明 VIRTIO_BLK_F_RO";

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    std::memset(f.mem() + data, 0xAA, 16);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 3) << "对只读后端写入应返回 RO 错误";
    EXPECT_EQ(f.image[0], 0) << "镜像不能被改动";
}

TEST(VirtioTest, UnsupportedRequestReturnsUnsupp) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 99, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.mem()[status], 2) << "未知请求类型应返回 UNSUPP";
}

TEST(VirtioTest, InterruptAckClearsLine) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 16, 0, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.read(0x060) & 1u, 1u) << "interrupt_status 应置 VRING 位";
    f.write(0x064, 1); // ack
    for (int i = 0; i < 2; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 0) << "ack 后中断线应撤销";
}

TEST(VirtioTest, StatusZeroResetsQueue) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x030, 0);
    f.write(0x038, 4);
    f.write(0x044, 1);
    f.write(0x070, 0);
    f.write(0x030, 0);
    EXPECT_EQ(f.read(0x044), 0u) << "status 写 0 应复位队列";
}

// virtio-rng：descriptor 是设备可写的数据段，随机数在 JIT 里生成
TEST(VirtioRng, FillsBuffer) {
    VirtioFixture<VirtioRng> f;
    const uint32_t desc = 0x100, avail = 0x200, used = 0x300, data = 0x400;
    f.putDesc(desc, 0, data, 8, 2, 0); // F_WRITE
    f.put16(avail + 0, 0);
    f.put16(avail + 2, 1);
    f.put16(avail + 4, 0);
    f.put16(used + 0, 0);
    std::memset(f.mem() + data, 0, 8);

    f.configureQueue(desc, avail, used, 8);
    f.write(0x070, 1); // status
    f.write(0x050, 0); // notify

    for (int i = 0; i < 400; i++)
        f.tick();

    uint64_t v = 0;
    std::memcpy(&v, f.mem() + data, 8);
    EXPECT_NE(v, 0u);
    EXPECT_EQ(f.get16(used + 2), 1); // used.idx（vring_used 的 +0 是 flags）
    EXPECT_EQ(f.get16(used + 4), 0); // used.ring[0].id
    EXPECT_EQ(f.usedLen(used, 0), 8u);
}

// 大缓冲区：256 字节要跨 4 轮 64 字节的主端口事务
TEST(VirtioRng, LargeBuffer) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 256, 2, 0);
    std::memset(f.mem() + s.data, 0, 256);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    for (int i = 0; i < 1500; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 256u);
    // 高 64 字节也要被写到，不能只写第一块
    uint64_t tail = 0;
    std::memcpy(&tail, f.mem() + s.data + 248, 8);
    EXPECT_NE(tail, 0u);
}

// 链式描述符：两个可写段，written 是两段之和
TEST(VirtioRng, ChainedDescriptors) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 3, 1);     // NEXT|WRITE
    f.putDesc(s.desc, 1, s.data + 8, 8, 2, 0); // WRITE
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    for (int i = 0; i < 800; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 16u);
}

// 队列没 ready 时不处理请求
TEST(VirtioRng, IgnoresWhenNotReady) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 2, 0);
    prepareQueue(f, s, 1);
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
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 4, 2, 0);
    f.putDesc(s.desc, 1, s.data + 16, 4, 2, 0);
    prepareQueue(f, s, 2);
    f.put16(s.avail + 6, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    for (int i = 0; i < 1200; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 2);
    EXPECT_EQ(f.usedLen(s.used, 0), 4u);
    EXPECT_EQ(f.usedLen(s.used, 1), 4u);
}

// virtio-input：宿主 push 的 evdev 事件由 JIT 搬进 guest buffer
TEST(VirtioInput, DeliversEvents) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0); // F_WRITE
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    // 没有事件时不能消费队列
    for (int i = 0; i < 120; i++)
        f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 0u);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1})); // EV_KEY, KEY_A, 按下
    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 0})); // 松开
    for (int i = 0; i < 900; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1u);
    EXPECT_EQ(f.usedLen(s.used, 0), 16u);

    uint16_t type = 0, code = 0;
    int32_t value = -1;
    std::memcpy(&type, f.mem() + s.data, 2);
    std::memcpy(&code, f.mem() + s.data + 2, 2);
    std::memcpy(&value, f.mem() + s.data + 4, 4);
    EXPECT_EQ(type, 1);
    EXPECT_EQ(code, 30);
    EXPECT_EQ(value, 1);

    std::memcpy(&value, f.mem() + s.data + 12, 4);
    EXPECT_EQ(value, 0);
}

// buffer 小于一个事件时要立刻以 len=0 完成，不能把队列卡死
TEST(VirtioInput, TinyBufferCompletes) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 4, 2, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1}));
    for (int i = 0; i < 600; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1u);
    EXPECT_EQ(f.usedLen(s.used, 0), 0u);
}

// 三个 8 字节 buffer 各装一个事件：队列连续分发，used.idx 走到 3
TEST(VirtioInput, MultipleBuffers) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    for (int i = 0; i < 3; i++) {
        f.putDesc(s.desc, (uint16_t) i, s.data + i * 16, 8, 2, 0);
        f.put16(s.avail + 4 + 2 * i, (uint16_t) i);
    }
    prepareQueue(f, s, 3);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1}));
    ASSERT_TRUE(f.dev->pushInputEvent({1, 31, 1}));
    ASSERT_TRUE(f.dev->pushInputEvent({1, 32, 1}));
    for (int i = 0; i < 1500; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 3);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(f.usedLen(s.used, i), 8u);
        uint16_t code = 0;
        std::memcpy(&code, f.mem() + s.data + i * 16 + 2, 2);
        EXPECT_EQ(code, 30 + i);
    }
}

// 事件环满时 push 返回 false；设备消费后又能继续 push
TEST(VirtioInput, RingFull) {
    VirtioFixture<VirtioInput> f;
    // 环容量 64，留一个空位，最多放 63 个
    int pushed = 0;
    while (f.dev->pushInputEvent({1, (uint16_t) pushed, 0}))
        pushed++;
    EXPECT_EQ(pushed, 63);

    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 2, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);
    for (int i = 0; i < 600; i++)
        f.tick();
    EXPECT_EQ(f.usedLen(s.used, 0), 8u);
    EXPECT_TRUE(f.dev->pushInputEvent({1, 99, 0}));
}

// 环回绕：推满再消费，事件顺序不能乱
TEST(VirtioInput, RingWraparound) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0);
    prepareQueue(f, s, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    // 先塞 40 个，设备每次取 8 个，取完后 tail 前进 40
    for (int i = 0; i < 40; i++)
        ASSERT_TRUE(f.dev->pushInputEvent({1, (uint16_t) i, 0}));
    for (int round = 0; round < 5; round++) {
        f.put16(s.avail + 2, (uint16_t) (round + 1));
        f.put16(s.avail + 4 + 2 * (round % 8), 0);
        for (int i = 0; i < 400; i++)
            f.tick();
    }
    EXPECT_EQ(f.get16(s.used + 2), 5);

    // 再塞 40 个（跨过环边界），code 应该继续递增
    for (int i = 40; i < 80; i++)
        ASSERT_TRUE(f.dev->pushInputEvent({1, (uint16_t) i, 0}));
    for (int round = 5; round < 10; round++) {
        f.put16(s.avail + 2, (uint16_t) (round + 1));
        f.put16(s.avail + 4 + 2 * (round % 8), 0);
        for (int i = 0; i < 400; i++)
            f.tick();
    }
    EXPECT_EQ(f.get16(s.used + 2), 10);
}

// virtio-net：驱动提交可写 buffer，宿主塞帧，设备写 12 字节头 + 帧体
TEST(VirtioNet, ReceivePacket) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0); // F_WRITE
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 0); // RX 是队列 0
    f.write(0x070, 1);
    f.write(0x050, 0);

    uint8_t pkt[16];
    for (int i = 0; i < 16; i++) pkt[i] = (uint8_t) (0xA0 + i);
    ASSERT_TRUE(f.dev->pushRxPacket(pkt, 16));
    for (int i = 0; i < 900; i++) f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 28u); // 12 字节头 + 16 字节帧
    for (int i = 0; i < 12; i++)
        EXPECT_EQ(f.mem()[s.data + i], 0) << "virtio_net_hdr 应全 0";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[s.data + 12 + i], (uint8_t) (0xA0 + i));
}

// buffer 装不下 12 字节头：以 len=0 完成，包留在环里等下一个 buffer
TEST(VirtioNet, TinyRxBufferKeepsPacket) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 2, 0); // 比 virtio_net_hdr 还小
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 0);
    f.write(0x070, 1);
    f.write(0x050, 0);

    uint8_t pkt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(f.dev->pushRxPacket(pkt, 8));
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 0u);
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(f.mem()[s.data + i], 0) << "buffer 太小，设备不该写入";

    // 换一个够大的 buffer，包还在，应该被写进去
    f.putDesc(s.desc, 1, s.data + 32, 64, 2, 0);
    f.put16(s.avail + 2, 2);
    f.put16(s.avail + 6, 1);
    f.write(0x050, 0);
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 2);
    EXPECT_EQ(f.usedLen(s.used, 1), 20u); // 12 + 8
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(f.mem()[s.data + 32 + 12 + i], pkt[i]);
}

// 发包：驱动提交 12 字节头 + 帧体，设备读出来交给宿主
TEST(VirtioNet, TransmitPacket) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 20, 0, 0); // 可读
    std::memset(f.mem() + s.data, 0, 12);
    for (int i = 0; i < 8; i++)
        f.mem()[s.data + 12 + i] = (uint8_t) (i + 1);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1); // TX 是队列 1
    f.write(0x070, 1);
    f.write(0x050, 1);

    for (int i = 0; i < 900; i++) f.tick();

    uint8_t out[64];
    const uint32_t n = f.dev->popTxPacket(out, sizeof(out));
    EXPECT_EQ(n, 8u) << "宿主应该拿到去掉 12 字节头的帧体";
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], (uint8_t) (i + 1));
    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 8u);
}

// 链式发包：头和数据在不同的描述符里
TEST(VirtioNet, TransmitChained) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 12, 3, 1);          // NEXT，只有头
    f.putDesc(s.desc, 1, s.data + 16, 6, 0, 0);      // 帧体
    f.put16(s.data, 0);                              // 头内容
    for (int i = 0; i < 6; i++)
        f.mem()[s.data + 16 + i] = (uint8_t) (0x10 + i);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1);
    f.write(0x070, 1);
    f.write(0x050, 1);

    for (int i = 0; i < 900; i++) f.tick();

    uint8_t out[64];
    const uint32_t n = f.dev->popTxPacket(out, sizeof(out));
    EXPECT_EQ(n, 6u);
    for (int i = 0; i < 6; i++)
        EXPECT_EQ(out[i], (uint8_t) (0x10 + i));
    EXPECT_EQ(f.usedLen(s.used, 0), 6u);
}

// 队列没 ready 时两个队列都不动
TEST(VirtioNet, IgnoresWhenNotReady) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.write(0x030, 0);
    f.write(0x038, 8);
    f.write(0x080, s.desc);
    f.write(0x090, s.avail);
    f.write(0x0A0, s.used);
    f.write(0x070, 1);
    f.write(0x050, 0);

    uint8_t pkt[4] = {1, 2, 3, 4};
    ASSERT_TRUE(f.dev->pushRxPacket(pkt, 4));
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 0u);
}

// RX 宿主环满时 push 返回 false；设备消费后又能继续
TEST(VirtioNet, RxRingFull) {
    VirtioFixture<VirtioNet> f;
    uint8_t pkt[4] = {1, 2, 3, 4};
    int pushed = 0;
    while (f.dev->pushRxPacket(pkt, 4))
        pushed++;
    EXPECT_EQ(pushed, 7); // 环容量 8，留一个空位

    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 0);
    f.write(0x070, 1);
    f.write(0x050, 0);
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.usedLen(s.used, 0), 16u);
    EXPECT_TRUE(f.dev->pushRxPacket(pkt, 4));
}
