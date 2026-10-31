// 各元件容易出错的边界场景，覆盖修复过的缺陷点
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/components/io/I2C.h>
#include <dcs/components/platform/PLIC.h>
#include <dcs/components/io/UART.h>
#include <dcs/components/platform/WishboneCache.h>
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

namespace {

// ---------------- UART ----------------
struct UartFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    UART *u = nullptr;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *rx, *ack, *tx;
    explicit UartFx(int baud = 4) {
        auto p = std::make_unique<UART>("u", baud);
        u = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat_i = c->createNet("dat_i");
        dat_o = c->createNet("dat_o"); we = c->createNet("we"); sel = c->createNet("sel");
        stb = c->createNet("stb"); cyc = c->createNet("cyc"); rx = c->createNet("rx");
        ack = c->createNet("ack"); tx = c->createNet("tx");
        c->connect(u, "clk", clk); c->connect(u, "wb_adr_i", adr); c->connect(u, "wb_dat_i", dat_i);
        c->connect(u, "wb_we_i", we); c->connect(u, "wb_sel_i", sel); c->connect(u, "wb_stb_i", stb);
        c->connect(u, "wb_cyc_i", cyc); c->connect(u, "rx", rx); c->connect(u, "wb_dat_o", dat_o);
        c->connect(u, "wb_ack_o", ack); c->connect(u, "tx", tx);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        c->setWire(clk->id(), {(uint8_t) 0});
        c->setWire(stb->id(), {(uint8_t) 0});
        c->setWire(cyc->id(), {(uint8_t) 0});
        c->setWire(we->id(), {(uint8_t) 0});
        c->setWire(rx->id(), {(uint8_t) 1});
        for (int i = 0; i < 2; ++i) tick();
    }
    void tick() {
        c->setWire(clk->id(), {(uint8_t) 0}); c->tick();
        c->setWire(clk->id(), {(uint8_t) 1}); c->tick();
    }
    uint32_t rd(uint32_t a) {
        c->setWire(adr->id(), {(uint8_t)(a & 0xFF), 0, 0, 0});
        c->setWire(we->id(), {(uint8_t) 0});
        c->setWire(stb->id(), {(uint8_t) 1}); c->setWire(cyc->id(), {(uint8_t) 1});
        for (int i = 0; i < 6; ++i) tick();
        auto v = c->getWire(dat_o->id());
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        for (int i = 0; i < 2; ++i) tick();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
};

TEST(EdgeCases, UartRejectsBadStopBit) {
    UartFx f(8);
    f.open();
    // 起始位 0 + 8 位数据 0x5A + 停止位写成 0（帧错误）
    const int bits[10] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0};
    for (int i = 0; i < 10; ++i) {
        f.c->setWire(f.rx->id(), {(uint8_t) bits[i]});
        for (int k = 0; k < 4; ++k) f.tick();
    }
    f.c->setWire(f.rx->id(), {(uint8_t) 1});
    for (int k = 0; k < 8; ++k) f.tick();
    EXPECT_EQ(f.rd(0x08) & 2u, 0u) << "停止位为 0 时不应交付数据";
}

TEST(EdgeCases, UartIgnoresNoisePulse) {
    UartFx f(8);
    f.open();
    // 一个远短于半位的低脉冲，应被判为噪声丢弃
    f.c->setWire(f.rx->id(), {(uint8_t) 0});
    f.tick();
    f.c->setWire(f.rx->id(), {(uint8_t) 1});
    for (int k = 0; k < 10; ++k) f.tick();
    EXPECT_EQ(f.rd(0x08) & 2u, 0u) << "短脉冲不应被当成起始位";
}

// ---------------- PLIC ----------------
struct PlicFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    PLIC *p = nullptr;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *src, *ack, *irq;
    explicit PlicFx(int n = 4) {
        auto q = std::make_unique<PLIC>("p", n);
        p = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat_i = c->createNet("dat_i");
        dat_o = c->createNet("dat_o"); we = c->createNet("we"); sel = c->createNet("sel");
        stb = c->createNet("stb"); cyc = c->createNet("cyc"); src = c->createNet("src");
        ack = c->createNet("ack"); irq = c->createNet("irq");
        c->connect(p, "clk", clk); c->connect(p, "wb_adr_i", adr); c->connect(p, "wb_dat_i", dat_i);
        c->connect(p, "wb_we_i", we); c->connect(p, "wb_sel_i", sel); c->connect(p, "wb_stb_i", stb);
        c->connect(p, "wb_cyc_i", cyc); c->connect(p, "irq_src", src);
        c->connect(p, "wb_dat_o", dat_o); c->connect(p, "wb_ack_o", ack); c->connect(p, "irq", irq);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        c->setWire(clk->id(), {(uint8_t) 0});
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        c->setWire(we->id(), {(uint8_t) 0});
        for (int i = 0; i < 2; ++i) tick();
    }
    void tick() {
        c->setWire(clk->id(), {(uint8_t) 0}); c->tick();
        c->setWire(clk->id(), {(uint8_t) 1}); c->tick();
    }
    void wr(uint32_t a, uint32_t v) {
        c->setWire(adr->id(), {(uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 24) & 0xFF)});
        c->setWire(dat_i->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)});
        c->setWire(we->id(), {(uint8_t) 1});
        c->setWire(stb->id(), {(uint8_t) 1}); c->setWire(cyc->id(), {(uint8_t) 1});
        for (int i = 0; i < 6; ++i) tick();
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        for (int i = 0; i < 4; ++i) tick();
    }
    uint32_t rd(uint32_t a) {
        c->setWire(adr->id(), {(uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 24) & 0xFF)});
        c->setWire(we->id(), {(uint8_t) 0});
        c->setWire(stb->id(), {(uint8_t) 1}); c->setWire(cyc->id(), {(uint8_t) 1});
        for (int i = 0; i < 8; ++i) tick();
        auto v = c->getWire(dat_o->id());
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        for (int i = 0; i < 4; ++i) tick();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    void setSrc(uint32_t m) { c->setWire(src->id(), {(uint8_t)(m & 0xFF)}); tick(); tick(); }
    bool irqHigh() { return c->getWire(irq->id())[0] != 0; }
};

TEST(EdgeCases, PlicCompleteAllowsRetrigger) {
    PlicFx f;
    f.open();
    f.wr(0x4, 4);        // source1 优先级 4
    f.wr(0x2000, 1u << 1); // 使能 source1（enable 是位图寄存器）
    f.setSrc(0x01);
    EXPECT_TRUE(f.irqHigh());
    EXPECT_EQ(f.rd(0x200004), 1u) << "claim 应取到 source1";
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh()) << "在服务中不应再次拉高中断";
    f.wr(0x200004, 1);   // complete
    f.setSrc(0x01);
    EXPECT_TRUE(f.irqHigh()) << "complete 后应能再次触发";
}

TEST(EdgeCases, PlicThresholdEqualityNotClaimed) {
    PlicFx f;
    f.open();
    f.wr(0x4, 3);
    f.wr(0x2000, 1u << 1);
    f.wr(0x200000, 3);   // 阈值等于优先级
    f.setSrc(0x01);
    EXPECT_EQ(f.rd(0x200004), 1u) << "claim 不受 threshold 影响，仍能取走源 1";
}

// ---------------- I2C 连续事务 ----------------
struct I2cFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    I2C *i = nullptr;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *sda_in, *ack, *scl, *sda_out, *sda_oe;
    explicit I2cFx(int d = 4) {
        auto p = std::make_unique<I2C>("i", d);
        i = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat_i = c->createNet("dat_i");
        dat_o = c->createNet("dat_o"); we = c->createNet("we"); sel = c->createNet("sel");
        stb = c->createNet("stb"); cyc = c->createNet("cyc"); sda_in = c->createNet("sda_in");
        ack = c->createNet("ack"); scl = c->createNet("scl"); sda_out = c->createNet("sda_out");
        sda_oe = c->createNet("sda_oe");
        c->connect(i, "clk", clk); c->connect(i, "wb_adr_i", adr); c->connect(i, "wb_dat_i", dat_i);
        c->connect(i, "wb_we_i", we); c->connect(i, "wb_sel_i", sel); c->connect(i, "wb_stb_i", stb);
        c->connect(i, "wb_cyc_i", cyc); c->connect(i, "sda_in", sda_in);
        c->connect(i, "wb_dat_o", dat_o); c->connect(i, "wb_ack_o", ack);
        c->connect(i, "scl", scl); c->connect(i, "sda_out", sda_out); c->connect(i, "sda_oe", sda_oe);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        c->setWire(clk->id(), {(uint8_t) 0});
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        c->setWire(we->id(), {(uint8_t) 0}); c->setWire(sda_in->id(), {(uint8_t) 1});
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() {
        c->setWire(clk->id(), {(uint8_t) 0}); c->tick();
        c->setWire(clk->id(), {(uint8_t) 1}); c->tick();
    }
    void wr(uint32_t a, uint32_t v) {
        c->setWire(adr->id(), {(uint8_t)(a & 0xFF), 0, 0, 0});
        c->setWire(dat_i->id(), {(uint8_t)(v & 0xFF), 0, 0, 0});
        c->setWire(we->id(), {(uint8_t) 1});
        c->setWire(stb->id(), {(uint8_t) 1}); c->setWire(cyc->id(), {(uint8_t) 1});
        for (int k = 0; k < 4; ++k) tick();
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        for (int k = 0; k < 2; ++k) tick();
    }
    uint32_t rd(uint32_t a) {
        c->setWire(adr->id(), {(uint8_t)(a & 0xFF), 0, 0, 0});
        c->setWire(we->id(), {(uint8_t) 0});
        c->setWire(stb->id(), {(uint8_t) 1}); c->setWire(cyc->id(), {(uint8_t) 1});
        for (int k = 0; k < 6; ++k) tick();
        auto v = c->getWire(dat_o->id());
        c->setWire(stb->id(), {(uint8_t) 0}); c->setWire(cyc->id(), {(uint8_t) 0});
        for (int k = 0; k < 2; ++k) tick();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
};

TEST(EdgeCases, I2cCanRunTwice) {
    I2cFx f(4);
    f.open();
    f.wr(0x10, 0x50);
    f.wr(0x00, 0xA5);
    f.wr(0x0C, 1);
    for (int k = 0; k < 200; ++k) f.tick();
    EXPECT_NE(f.rd(0x08) & 2u, 0u) << "第一次事务应完成";
    f.wr(0x0C, 1); // 再次启动
    for (int k = 0; k < 200; ++k) f.tick();
    EXPECT_NE(f.rd(0x08) & 2u, 0u) << "DONE 之后必须还能再发起一次事务";
}

// ---------------- 缓存字节写 ----------------
struct CacheFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    WishboneCache *cache = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk, *c_adr, *c_dat_o, *c_dat_i, *c_we, *c_sel, *c_stb, *c_cyc, *c_ack;
    Net *m_adr, *m_dat_o, *m_dat_i, *m_we, *m_sel, *m_stb, *m_cyc, *m_ack;
    CacheFx() {
        auto p = std::make_unique<WishboneCache>("cache", 16);
        cache = p.get();
        c->addComponent(std::move(p));
        auto q = std::make_unique<WishboneMemory>("mem", 16, 32);
        mem = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk");
        c_adr = c->createNet("c_adr"); c_dat_o = c->createNet("c_dat_o"); c_dat_i = c->createNet("c_dat_i");
        c_we = c->createNet("c_we"); c_sel = c->createNet("c_sel"); c_stb = c->createNet("c_stb");
        c_cyc = c->createNet("c_cyc"); c_ack = c->createNet("c_ack");
        m_adr = c->createNet("m_adr"); m_dat_o = c->createNet("m_dat_o"); m_dat_i = c->createNet("m_dat_i");
        m_we = c->createNet("m_we"); m_sel = c->createNet("m_sel"); m_stb = c->createNet("m_stb");
        m_cyc = c->createNet("m_cyc"); m_ack = c->createNet("m_ack");
        c->connect(cache, "wb_adr_i", c_adr); c->connect(cache, "wb_dat_i", c_dat_o);
        c->connect(cache, "wb_we_i", c_we); c->connect(cache, "wb_sel_i", c_sel);
        c->connect(cache, "wb_stb_i", c_stb); c->connect(cache, "wb_cyc_i", c_cyc);
        c->connect(cache, "wb_dat_o", c_dat_i); c->connect(cache, "wb_ack_o", c_ack);
        c->connect(cache, "wb_adr_o", m_adr); c->connect(cache, "wb_dat_i_o", m_dat_o);
        c->connect(cache, "wb_we_o", m_we); c->connect(cache, "wb_sel_o", m_sel);
        c->connect(cache, "wb_stb_o", m_stb); c->connect(cache, "wb_cyc_o", m_cyc);
        c->connect(cache, "wb_dat_o_i", m_dat_i); c->connect(cache, "wb_ack_o_i", m_ack);
        c->connect(mem, "clk", clk); c->connect(mem, "wb_adr_i", m_adr);
        c->connect(mem, "wb_dat_i", m_dat_o); c->connect(mem, "wb_we_i", m_we);
        c->connect(mem, "wb_sel_i", m_sel); c->connect(mem, "wb_stb_i", m_stb);
        c->connect(mem, "wb_cyc_i", m_cyc); c->connect(mem, "wb_dat_o", m_dat_i);
        c->connect(mem, "wb_ack_o", m_ack);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        c->setWire(clk->id(), {(uint8_t) 0});
        c->setWire(c_stb->id(), {(uint8_t) 0}); c->setWire(c_cyc->id(), {(uint8_t) 0});
        c->setWire(c_we->id(), {(uint8_t) 0}); c->setWire(c_sel->id(), {(uint8_t) 0x0F});
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() {
        c->setWire(clk->id(), {(uint8_t) 0}); c->tick();
        c->setWire(clk->id(), {(uint8_t) 1}); c->tick();
    }
    uint32_t access(uint32_t a, bool we, uint32_t d, uint8_t s = 0x0F) {
        c->setWire(c_adr->id(), {(uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF), 0, 0});
        c->setWire(c_dat_o->id(), {(uint8_t)(d & 0xFF), (uint8_t)((d >> 8) & 0xFF), (uint8_t)((d >> 16) & 0xFF), (uint8_t)((d >> 24) & 0xFF)});
        c->setWire(c_we->id(), {(uint8_t)(we ? 1 : 0)});
        c->setWire(c_sel->id(), {s});
        c->setWire(c_stb->id(), {(uint8_t) 1}); c->setWire(c_cyc->id(), {(uint8_t) 1});
        uint32_t r = 0;
        for (int k = 0; k < 60; ++k) {
            tick();
            if (c->getWire(c_ack->id())[0]) {
                auto v = c->getWire(c_dat_i->id());
                r = uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
                break;
            }
        }
        c->setWire(c_stb->id(), {(uint8_t) 0}); c->setWire(c_cyc->id(), {(uint8_t) 0});
        for (int k = 0; k < 3; ++k) tick();
        return r;
    }
    uint32_t peek(uint32_t a) { uint32_t v; std::memcpy(&v, mem->writableMemPtr() + a, 4); return v; }
    void put(uint32_t a, uint32_t v) { std::memcpy(mem->writableMemPtr() + a, &v, 4); }
};

TEST(EdgeCases, CacheByteWriteKeepsOtherBytes) {
    CacheFx f;
    f.open();
    f.put(0x100, 0x11223344);
    EXPECT_EQ(f.access(0x100, false, 0), 0x11223344u); // 先把该行读进来
    f.access(0x100, true, 0x0000AA00, 0x02);           // 只写第 2 个字节
    EXPECT_EQ(f.peek(0x100), 0x1122AA44u) << "字节写只应改选中字节";
    EXPECT_EQ(f.access(0x100, false, 0), 0x1122AA44u) << "缓存里的副本也要一致";
}

TEST(EdgeCases, CacheDifferentLinesDoNotAlias) {
    CacheFx f;
    f.open();
    f.put(0x200, 0xAAAAAAAA);
    f.put(0x300, 0xBBBBBBBB);
    EXPECT_EQ(f.access(0x200, false, 0), 0xAAAAAAAAu);
    EXPECT_EQ(f.access(0x300, false, 0), 0xBBBBBBBBu) << "不同行不应互相覆盖";
    EXPECT_EQ(f.access(0x200, false, 0), 0xAAAAAAAAu);
}

} // namespace
