// 补齐此前缺的测试场景：SPI 位序、DMA 多次搬运、AXI 字节使能与背压、
// Interconnect 多主仲裁、缓存行替换
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/components/platform/AxiLiteBridge.h>
#include <dcs/components/platform/DMA.h>
#include <dcs/components/io/SPI.h>
#include <dcs/components/platform/WishboneCache.h>
#include <dcs/components/platform/WishboneInterconnect.h>
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

namespace {

// ---------------- SPI 位序与片选 ----------------
struct SpiFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    SPI *s = nullptr;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *miso, *ack, *sclk, *mosi, *cs;
    explicit SpiFx(int d = 8) {
        auto p = std::make_unique<SPI>("spi", d);
        s = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat_i = c->createNet("dat_i");
        dat_o = c->createNet("dat_o"); we = c->createNet("we"); sel = c->createNet("sel");
        stb = c->createNet("stb"); cyc = c->createNet("cyc"); miso = c->createNet("miso");
        ack = c->createNet("ack"); sclk = c->createNet("sclk"); mosi = c->createNet("mosi");
        cs = c->createNet("cs");
        c->connect(s, "clk", clk); c->connect(s, "wb_adr_i", adr); c->connect(s, "wb_dat_i", dat_i);
        c->connect(s, "wb_we_i", we); c->connect(s, "wb_sel_i", sel); c->connect(s, "wb_stb_i", stb);
        c->connect(s, "wb_cyc_i", cyc); c->connect(s, "miso", miso);
        c->connect(s, "wb_dat_o", dat_o); c->connect(s, "wb_ack_o", ack);
        c->connect(s, "sclk", sclk); c->connect(s, "mosi", mosi); c->connect(s, "cs", cs);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        for (auto *n : {clk, stb, cyc, we, miso}) c->setWire(n->id(), (uint8_t) 0);
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() {
        c->setWire(clk->id(), (uint8_t) 0); c->tick();
        c->setWire(clk->id(), (uint8_t) 1); c->tick();
    }
    void wr(uint32_t a, uint32_t v) {
        c->setWire(adr->id(), {(uint8_t)(a & 0xFF), 0, 0, 0});
        c->setWire(dat_i->id(), {(uint8_t)(v & 0xFF), 0, 0, 0});
        c->setWire(we->id(), (uint8_t) 1);
        c->setWire(stb->id(), (uint8_t) 1); c->setWire(cyc->id(), (uint8_t) 1);
        for (int k = 0; k < 4; ++k) tick();
        c->setWire(stb->id(), (uint8_t) 0); c->setWire(cyc->id(), (uint8_t) 0);
        for (int k = 0; k < 2; ++k) tick();
    }
    bool bit(Net *n) { return c->getWire(n->id())[0] != 0; }
};

TEST(Coverage, SpiSendsMsbFirst) {
    SpiFx f(8);
    f.open();
    f.wr(0x00, 0x81); // 1000 0001
    f.wr(0x0C, 1);
    std::vector<int> bits;
    bool lastSclk = false;
    for (int k = 0; k < 400; ++k) {
        f.tick();
        if (f.bit(f.sclk) && !lastSclk) bits.push_back(f.bit(f.mosi) ? 1 : 0); // 上升沿采样
        lastSclk = f.bit(f.sclk);
        if (bits.size() == 8) break;
    }
    ASSERT_EQ(bits.size(), 8u);
    const int expect[8] = {1, 0, 0, 0, 0, 0, 0, 1};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(bits[i], expect[i]) << "第 " << i << " 位";
}

TEST(Coverage, SpiCsLowDuringTransfer) {
    SpiFx f(8);
    f.open();
    EXPECT_TRUE(f.bit(f.cs)) << "空闲时片选为高";
    f.wr(0x00, 0xFF);
    f.wr(0x0C, 1);
    f.tick();
    EXPECT_FALSE(f.bit(f.cs)) << "传输期间片选拉低";
    for (int k = 0; k < 400; ++k) f.tick();
    EXPECT_TRUE(f.bit(f.cs)) << "结束后片选恢复";
}

// ---------------- DMA 多次搬运与长度对齐 ----------------
struct DmaFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    DMA *d = nullptr;
    WishboneMemory *m = nullptr;
    Net *clk, *c_adr, *c_dat_o, *c_dat_i, *c_we, *c_sel, *c_stb, *c_cyc, *c_ack;
    Net *m_adr, *m_dat_o, *m_dat_i, *m_we, *m_sel, *m_stb, *m_cyc, *m_ack;
    DmaFx() {
        auto p = std::make_unique<DMA>("dma");
        d = p.get();
        c->addComponent(std::move(p));
        auto q = std::make_unique<WishboneMemory>("mem", 16, 32);
        m = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk");
        c_adr = c->createNet("c_adr"); c_dat_o = c->createNet("c_dat_o"); c_dat_i = c->createNet("c_dat_i");
        c_we = c->createNet("c_we"); c_sel = c->createNet("c_sel"); c_stb = c->createNet("c_stb");
        c_cyc = c->createNet("c_cyc"); c_ack = c->createNet("c_ack");
        m_adr = c->createNet("m_adr"); m_dat_o = c->createNet("m_dat_o"); m_dat_i = c->createNet("m_dat_i");
        m_we = c->createNet("m_we"); m_sel = c->createNet("m_sel"); m_stb = c->createNet("m_stb");
        m_cyc = c->createNet("m_cyc"); m_ack = c->createNet("m_ack");
        c->connect(d, "cfg_adr_i", c_adr); c->connect(d, "cfg_dat_i", c_dat_o);
        c->connect(d, "cfg_we_i", c_we); c->connect(d, "cfg_sel_i", c_sel);
        c->connect(d, "cfg_stb_i", c_stb); c->connect(d, "cfg_cyc_i", c_cyc);
        c->connect(d, "cfg_dat_o", c_dat_i); c->connect(d, "cfg_ack_o", c_ack);
        c->connect(d, "m_adr_o", m_adr); c->connect(d, "m_dat_o", m_dat_o);
        c->connect(d, "m_we_o", m_we); c->connect(d, "m_sel_o", m_sel);
        c->connect(d, "m_stb_o", m_stb); c->connect(d, "m_cyc_o", m_cyc);
        c->connect(d, "m_dat_i", m_dat_i); c->connect(d, "m_ack_i", m_ack);
        c->connect(m, "clk", clk); c->connect(m, "wb_adr_i", m_adr);
        c->connect(m, "wb_dat_i", m_dat_o); c->connect(m, "wb_we_i", m_we);
        c->connect(m, "wb_sel_i", m_sel); c->connect(m, "wb_stb_i", m_stb);
        c->connect(m, "wb_cyc_i", m_cyc); c->connect(m, "wb_dat_o", m_dat_i);
        c->connect(m, "wb_ack_o", m_ack);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        for (auto *n : {clk, c_stb, c_cyc, c_we}) c->setWire(n->id(), (uint8_t) 0);
        c->setWire(c_sel->id(), (uint8_t) 0x0F);
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() {
        c->setWire(clk->id(), (uint8_t) 0); c->tick();
        c->setWire(clk->id(), (uint8_t) 1); c->tick();
    }
    void wr(uint32_t a, uint32_t v) {
        c->setWire(c_adr->id(), {(uint8_t)(a & 0xFF), 0, 0, 0});
        c->setWire(c_dat_o->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)});
        c->setWire(c_we->id(), (uint8_t) 1);
        c->setWire(c_stb->id(), (uint8_t) 1); c->setWire(c_cyc->id(), (uint8_t) 1);
        for (int k = 0; k < 4; ++k) tick();
        c->setWire(c_stb->id(), (uint8_t) 0); c->setWire(c_cyc->id(), (uint8_t) 0);
        for (int k = 0; k < 2; ++k) tick();
    }
    void put(uint32_t a, uint32_t v) { std::memcpy(m->writableMemPtr() + a, &v, 4); }
    uint32_t peek(uint32_t a) { uint32_t v; std::memcpy(&v, m->writableMemPtr() + a, 4); return v; }
};

TEST(Coverage, DmaCanRunTwice) {
    DmaFx f;
    f.open();
    f.put(0x100, 0x11111111);
    f.put(0x200, 0);
    f.wr(0x00, 0x100); f.wr(0x04, 0x200); f.wr(0x08, 4); f.wr(0x0C, 1);
    for (int k = 0; k < 80; ++k) f.tick();
    EXPECT_EQ(f.peek(0x200), 0x11111111u);
    f.put(0x104, 0x22222222);
    f.put(0x204, 0);
    f.wr(0x00, 0x104); f.wr(0x04, 0x204); f.wr(0x08, 4); f.wr(0x0C, 1);
    for (int k = 0; k < 80; ++k) f.tick();
    EXPECT_EQ(f.peek(0x204), 0x22222222u) << "完成之后必须能再搬一次";
}

TEST(Coverage, DmaRoundsLengthDownToWord) {
    DmaFx f;
    f.open();
    f.put(0x300, 0xAAAA0001);
    f.put(0x304, 0xBBBB0002);
    f.put(0x400, 0); f.put(0x404, 0);
    f.wr(0x00, 0x300); f.wr(0x04, 0x400); f.wr(0x08, 6); f.wr(0x0C, 1); // 6 字节 -> 按 4 对齐只搬 1 字
    for (int k = 0; k < 120; ++k) f.tick();
    EXPECT_EQ(f.peek(0x400), 0xAAAA0001u) << "应对齐到 4 字节搬一个字";
    EXPECT_EQ(f.peek(0x404), 0u) << "不足一个字的部分不应被搬";
}

// ---------------- AXI 字节使能与背压 ----------------
struct AxiFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    AxiLiteBridge *b = nullptr;
    WishboneMemory *m = nullptr;
    Net *clk, *awaddr, *awvalid, *wdata, *wstrb, *wvalid, *bready, *araddr, *arvalid, *rready;
    Net *awready, *wready, *bresp, *bvalid, *arready, *rdata, *rresp, *rvalid;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    AxiFx() {
        auto p = std::make_unique<AxiLiteBridge>("br");
        b = p.get();
        c->addComponent(std::move(p));
        auto q = std::make_unique<WishboneMemory>("mem", 16, 32);
        m = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk");
        awaddr = c->createNet("aw"); awvalid = c->createNet("awv"); wdata = c->createNet("wd");
        wstrb = c->createNet("ws"); wvalid = c->createNet("wv"); bready = c->createNet("br");
        araddr = c->createNet("ar"); arvalid = c->createNet("arv"); rready = c->createNet("rr");
        awready = c->createNet("awr"); wready = c->createNet("wr"); bresp = c->createNet("bs");
        bvalid = c->createNet("bv"); arready = c->createNet("arr"); rdata = c->createNet("rd");
        rresp = c->createNet("rs"); rvalid = c->createNet("rv");
        wb_adr = c->createNet("wa"); wb_dat_o = c->createNet("wdo"); wb_dat_i = c->createNet("wdi");
        wb_we = c->createNet("wwe"); wb_sel = c->createNet("wsel"); wb_stb = c->createNet("wstb");
        wb_cyc = c->createNet("wcyc"); wb_ack = c->createNet("wack");
        c->connect(b, "clk", clk);
        c->connect(b, "s_axi_awaddr", awaddr); c->connect(b, "s_axi_awvalid", awvalid);
        c->connect(b, "s_axi_wdata", wdata); c->connect(b, "s_axi_wstrb", wstrb);
        c->connect(b, "s_axi_wvalid", wvalid); c->connect(b, "s_axi_bready", bready);
        c->connect(b, "s_axi_araddr", araddr); c->connect(b, "s_axi_arvalid", arvalid);
        c->connect(b, "s_axi_rready", rready); c->connect(b, "wb_dat_i", wb_dat_i);
        c->connect(b, "wb_ack_i", wb_ack);
        c->connect(b, "s_axi_awready", awready); c->connect(b, "s_axi_wready", wready);
        c->connect(b, "s_axi_bresp", bresp); c->connect(b, "s_axi_bvalid", bvalid);
        c->connect(b, "s_axi_arready", arready); c->connect(b, "s_axi_rdata", rdata);
        c->connect(b, "s_axi_rresp", rresp); c->connect(b, "s_axi_rvalid", rvalid);
        c->connect(b, "wb_adr_o", wb_adr); c->connect(b, "wb_dat_o", wb_dat_o);
        c->connect(b, "wb_we_o", wb_we); c->connect(b, "wb_sel_o", wb_sel);
        c->connect(b, "wb_stb_o", wb_stb); c->connect(b, "wb_cyc_o", wb_cyc);
        c->connect(m, "clk", clk); c->connect(m, "wb_adr_i", wb_adr);
        c->connect(m, "wb_dat_i", wb_dat_o); c->connect(m, "wb_we_i", wb_we);
        c->connect(m, "wb_sel_i", wb_sel); c->connect(m, "wb_stb_i", wb_stb);
        c->connect(m, "wb_cyc_i", wb_cyc); c->connect(m, "wb_dat_o", wb_dat_i);
        c->connect(m, "wb_ack_o", wb_ack);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        for (auto *n : {clk, awvalid, wvalid, arvalid, bready, rready}) c->setWire(n->id(), (uint8_t) 0);
        c->setWire(wstrb->id(), (uint8_t) 0x0F);
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() {
        c->setWire(clk->id(), (uint8_t) 0); c->tick();
        c->setWire(clk->id(), (uint8_t) 1); c->tick();
    }
    bool bit(Net *n) { return c->getWire(n->id())[0] != 0; }
    uint32_t peek(uint32_t a) { uint32_t v; std::memcpy(&v, m->writableMemPtr() + a, 4); return v; }
    void put(uint32_t a, uint32_t v) { std::memcpy(m->writableMemPtr() + a, &v, 4); }
};

TEST(Coverage, AxiByteStrobeOnlyWritesSelectedBytes) {
    AxiFx f;
    f.open();
    f.put(0x40, 0x11223344);
    f.c->setWire(f.awaddr->id(), {(uint8_t) 0x40, 0, 0, 0});
    f.c->setWire(f.wdata->id(), {(uint8_t) 0xAA, (uint8_t) 0xBB, 0, 0});
    f.c->setWire(f.wstrb->id(), (uint8_t) 0x03); // 只写低两个字节
    f.c->setWire(f.awvalid->id(), (uint8_t) 1);
    f.c->setWire(f.wvalid->id(), (uint8_t) 1);
    f.c->setWire(f.bready->id(), (uint8_t) 1);
    for (int k = 0; k < 40; ++k) f.tick();
    EXPECT_EQ(f.peek(0x40), 0x1122BBAAu) << "只应改选中的两个字节";
}

TEST(Coverage, AxiBvalidHoldsUntilReady) {
    AxiFx f;
    f.open();
    f.put(0x50, 0);
    f.c->setWire(f.awaddr->id(), {(uint8_t) 0x50, 0, 0, 0});
    f.c->setWire(f.wdata->id(), {(uint8_t) 0x5A, 0, 0, 0});
    f.c->setWire(f.wstrb->id(), (uint8_t) 0x0F);
    f.c->setWire(f.awvalid->id(), (uint8_t) 1);
    f.c->setWire(f.wvalid->id(), (uint8_t) 1);
    f.c->setWire(f.bready->id(), (uint8_t) 0); // 先不给 ready
    bool sawBvalid = false;
    for (int k = 0; k < 40; ++k) {
        f.tick();
        if (f.bit(f.bvalid)) sawBvalid = true;
    }
    EXPECT_TRUE(sawBvalid);
    EXPECT_TRUE(f.bit(f.bvalid)) << "bready 为 0 时 bvalid 必须保持";
    f.c->setWire(f.bready->id(), (uint8_t) 1);
    for (int k = 0; k < 10; ++k) f.tick();
    EXPECT_FALSE(f.bit(f.bvalid)) << "握手完成后 bvalid 应撤销";
}

// ---------------- Interconnect 多主仲裁 ----------------
struct IcFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    WishboneInterconnect *ic = nullptr;
    WishboneMemory *m0 = nullptr, *m1 = nullptr;
    Net *a0, *d0o, *d0i, *w0, *s0, *st0, *cy0, *ak0;
    Net *a1, *d1o, *d1i, *w1, *s1, *st1, *cy1, *ak1;
    Net *s0a, *s0d, *s0w, *s0s, *s0st, *s0cy, *s0ak, *s0do;
    IcFx() {
        auto p = std::make_unique<WishboneInterconnect>("ic", "0:65536", 2);
        ic = p.get();
        c->addComponent(std::move(p));
        auto q0 = std::make_unique<WishboneMemory>("mem0", 16, 32);
        m0 = q0.get();
        c->addComponent(std::move(q0));
        a0 = c->createNet("a0"); d0o = c->createNet("d0o"); d0i = c->createNet("d0i");
        w0 = c->createNet("w0"); s0 = c->createNet("s0"); st0 = c->createNet("st0"); cy0 = c->createNet("cy0"); ak0 = c->createNet("ak0");
        a1 = c->createNet("a1"); d1o = c->createNet("d1o"); d1i = c->createNet("d1i");
        w1 = c->createNet("w1"); s1 = c->createNet("s1"); st1 = c->createNet("st1"); cy1 = c->createNet("cy1"); ak1 = c->createNet("ak1");
        s0a = c->createNet("s0a"); s0d = c->createNet("s0d"); s0w = c->createNet("s0w");
        s0s = c->createNet("s0s"); s0st = c->createNet("s0st"); s0cy = c->createNet("s0cy");
        s0ak = c->createNet("s0ak"); s0do = c->createNet("s0do");
        c->connect(ic, "m0_adr_i", a0); c->connect(ic, "m0_dat_i", d0o); c->connect(ic, "m0_we_i", w0);
        c->connect(ic, "m0_sel_i", s0); c->connect(ic, "m0_stb_i", st0); c->connect(ic, "m0_cyc_i", cy0);
        c->connect(ic, "m0_dat_o", d0i); c->connect(ic, "m0_ack_o", ak0);
        c->connect(ic, "m1_adr_i", a1); c->connect(ic, "m1_dat_i", d1o); c->connect(ic, "m1_we_i", w1);
        c->connect(ic, "m1_sel_i", s1); c->connect(ic, "m1_stb_i", st1); c->connect(ic, "m1_cyc_i", cy1);
        c->connect(ic, "m1_dat_o", d1i); c->connect(ic, "m1_ack_o", ak1);
        c->connect(ic, "s0_adr_o", s0a); c->connect(ic, "s0_dat_o", s0d); c->connect(ic, "s0_we_o", s0w);
        c->connect(ic, "s0_sel_o", s0s); c->connect(ic, "s0_stb_o", s0st); c->connect(ic, "s0_cyc_o", s0cy);
        c->connect(ic, "s0_dat_i", s0do); c->connect(ic, "s0_ack_i", s0ak);
        c->connect(m0, "wb_adr_i", s0a); c->connect(m0, "wb_dat_i", s0d); c->connect(m0, "wb_we_i", s0w);
        c->connect(m0, "wb_sel_i", s0s); c->connect(m0, "wb_stb_i", s0st); c->connect(m0, "wb_cyc_i", s0cy);
        c->connect(m0, "wb_dat_o", s0do); c->connect(m0, "wb_ack_o", s0ak);
    }
    void open() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        for (auto *n : {st0, cy0, w0, st1, cy1, w1}) c->setWire(n->id(), (uint8_t) 0);
        c->setWire(s0->id(), (uint8_t) 0x0F);
        c->setWire(s1->id(), (uint8_t) 0x0F);
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() { c->tick(); }
    void put(uint32_t a, uint32_t v) { std::memcpy(m0->writableMemPtr() + a, &v, 4); }
    uint32_t word(Net *n) {
        auto v = c->getWire(n->id());
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    bool bit(Net *n) { return c->getWire(n->id())[0] != 0; }
};

TEST(Coverage, InterconnectArbitratesBetweenMasters) {
    IcFx f;
    f.open();
    f.put(0x10, 0xAAAA1111);
    f.put(0x20, 0xBBBB2222);
    // 两个主设备同时读不同地址
    f.c->setWire(f.a0->id(), {(uint8_t) 0x10, 0, 0, 0});
    f.c->setWire(f.a1->id(), {(uint8_t) 0x20, 0, 0, 0});
    f.c->setWire(f.st0->id(), (uint8_t) 1); f.c->setWire(f.cy0->id(), (uint8_t) 1);
    f.c->setWire(f.st1->id(), (uint8_t) 1); f.c->setWire(f.cy1->id(), (uint8_t) 1);
    uint32_t got0 = 0, got1 = 0;
    for (int k = 0; k < 60; ++k) {
        f.tick();
        if (f.bit(f.ak0)) got0 = f.word(f.d0i);
        if (f.bit(f.ak1)) got1 = f.word(f.d1i);
    }
    // 两个主设备都应拿到各自地址的数据，不能串
    if (got0) EXPECT_EQ(got0, 0xAAAA1111u) << "主设备 0 拿到的必须是自己的数据";
    if (got1) EXPECT_EQ(got1, 0xBBBB2222u) << "主设备 1 拿到的必须是自己的数据";
    EXPECT_TRUE(got0 || got1) << "至少应有一个主设备完成事务";
}

TEST(Coverage, InterconnectSingleMasterUnaffected) {
    IcFx f;
    f.open();
    f.put(0x30, 0xCCCC3333);
    f.c->setWire(f.a0->id(), {(uint8_t) 0x30, 0, 0, 0});
    f.c->setWire(f.st0->id(), (uint8_t) 1); f.c->setWire(f.cy0->id(), (uint8_t) 1);
    uint32_t got = 0;
    for (int k = 0; k < 60; ++k) {
        f.tick();
        if (f.bit(f.ak0)) got = f.word(f.d0i);
    }
    EXPECT_EQ(got, 0xCCCC3333u) << "只有一个主设备时不应受仲裁影响";
}

// ---------------- 缓存行替换 ----------------
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
        c_adr = c->createNet("ca"); c_dat_o = c->createNet("cdo"); c_dat_i = c->createNet("cdi");
        c_we = c->createNet("cw"); c_sel = c->createNet("cs"); c_stb = c->createNet("cst");
        c_cyc = c->createNet("ccy"); c_ack = c->createNet("cak");
        m_adr = c->createNet("ma"); m_dat_o = c->createNet("mdo"); m_dat_i = c->createNet("mdi");
        m_we = c->createNet("mw"); m_sel = c->createNet("ms"); m_stb = c->createNet("mst");
        m_cyc = c->createNet("mcy"); m_ack = c->createNet("mak");
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
        for (auto *n : {clk, c_stb, c_cyc, c_we}) c->setWire(n->id(), (uint8_t) 0);
        c->setWire(c_sel->id(), (uint8_t) 0x0F);
        for (int k = 0; k < 2; ++k) tick();
    }
    void tick() {
        c->setWire(clk->id(), (uint8_t) 0); c->tick();
        c->setWire(clk->id(), (uint8_t) 1); c->tick();
    }
    uint32_t access(uint32_t a, bool we, uint32_t d) {
        c->setWire(c_adr->id(), {(uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF), 0, 0});
        c->setWire(c_dat_o->id(), {(uint8_t)(d & 0xFF), (uint8_t)((d >> 8) & 0xFF), (uint8_t)((d >> 16) & 0xFF), (uint8_t)((d >> 24) & 0xFF)});
        c->setWire(c_we->id(), (uint8_t)(we ? 1 : 0));
        c->setWire(c_stb->id(), (uint8_t) 1); c->setWire(c_cyc->id(), (uint8_t) 1);
        uint32_t r = 0;
        for (int k = 0; k < 60; ++k) {
            tick();
            if (c->getWire(c_ack->id())[0]) {
                auto v = c->getWire(c_dat_i->id());
                r = uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
                break;
            }
        }
        c->setWire(c_stb->id(), (uint8_t) 0); c->setWire(c_cyc->id(), (uint8_t) 0);
        for (int k = 0; k < 3; ++k) tick();
        return r;
    }
    void put(uint32_t a, uint32_t v) { std::memcpy(mem->writableMemPtr() + a, &v, 4); }
};

TEST(Coverage, CacheLineReplacement) {
    CacheFx f;
    f.open();
    f.put(0x100, 0x01010101);
    f.put(0x200, 0x02020202); // 相隔 0x100 = 16 行 * 16 字节，映射到同一行索引
    EXPECT_EQ(f.access(0x100, false, 0), 0x01010101u);
    EXPECT_EQ(f.access(0x200, false, 0), 0x02020202u);
    EXPECT_EQ(f.access(0x100, false, 0), 0x01010101u) << "被替换掉的行再次访问必须重新填充";
}

} // namespace
