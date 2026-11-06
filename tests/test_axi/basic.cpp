// AXI4-Lite 桥：写事务、读事务、握手信号
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/components/platform/AxiLiteBridge.h>
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    AxiLiteBridge *br = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk = nullptr;
    Net *awaddr = nullptr, *awvalid = nullptr, *wdata = nullptr, *wstrb = nullptr, *wvalid = nullptr, *bready = nullptr;
    Net *araddr = nullptr, *arvalid = nullptr, *rready = nullptr;
    Net *awready = nullptr, *wready = nullptr, *bresp = nullptr, *bvalid = nullptr;
    Net *arready = nullptr, *rdata = nullptr, *rresp = nullptr, *rvalid = nullptr;
    Net *wb_adr = nullptr, *wb_dat_o = nullptr, *wb_dat_i = nullptr, *wb_we = nullptr;
    Net *wb_sel = nullptr, *wb_stb = nullptr, *wb_cyc = nullptr, *wb_ack = nullptr;

    Fixture() {
        auto b = std::make_unique<AxiLiteBridge>("br0");
        br = b.get();
        circuit->addComponent(std::move(b));
        auto m = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = m.get();
        circuit->addComponent(std::move(m));
        clk = circuit->createNet("clk");
        awaddr = circuit->createNet("awaddr"); awvalid = circuit->createNet("awvalid");
        wdata = circuit->createNet("wdata"); wstrb = circuit->createNet("wstrb");
        wvalid = circuit->createNet("wvalid"); bready = circuit->createNet("bready");
        araddr = circuit->createNet("araddr"); arvalid = circuit->createNet("arvalid");
        rready = circuit->createNet("rready");
        awready = circuit->createNet("awready"); wready = circuit->createNet("wready");
        bresp = circuit->createNet("bresp"); bvalid = circuit->createNet("bvalid");
        arready = circuit->createNet("arready"); rdata = circuit->createNet("rdata");
        rresp = circuit->createNet("rresp"); rvalid = circuit->createNet("rvalid");
        wb_adr = circuit->createNet("wb_adr"); wb_dat_o = circuit->createNet("wb_dat_o");
        wb_dat_i = circuit->createNet("wb_dat_i"); wb_we = circuit->createNet("wb_we");
        wb_sel = circuit->createNet("wb_sel"); wb_stb = circuit->createNet("wb_stb");
        wb_cyc = circuit->createNet("wb_cyc"); wb_ack = circuit->createNet("wb_ack");

        circuit->connect(br, "clk", clk);
        circuit->connect(br, "s_axi_awaddr", awaddr);
        circuit->connect(br, "s_axi_awvalid", awvalid);
        circuit->connect(br, "s_axi_wdata", wdata);
        circuit->connect(br, "s_axi_wstrb", wstrb);
        circuit->connect(br, "s_axi_wvalid", wvalid);
        circuit->connect(br, "s_axi_bready", bready);
        circuit->connect(br, "s_axi_araddr", araddr);
        circuit->connect(br, "s_axi_arvalid", arvalid);
        circuit->connect(br, "s_axi_rready", rready);
        circuit->connect(br, "wb_dat_i", wb_dat_i);
        circuit->connect(br, "wb_ack_i", wb_ack);
        circuit->connect(br, "s_axi_awready", awready);
        circuit->connect(br, "s_axi_wready", wready);
        circuit->connect(br, "s_axi_bresp", bresp);
        circuit->connect(br, "s_axi_bvalid", bvalid);
        circuit->connect(br, "s_axi_arready", arready);
        circuit->connect(br, "s_axi_rdata", rdata);
        circuit->connect(br, "s_axi_rresp", rresp);
        circuit->connect(br, "s_axi_rvalid", rvalid);
        circuit->connect(br, "wb_adr_o", wb_adr);
        circuit->connect(br, "wb_dat_o", wb_dat_o);
        circuit->connect(br, "wb_we_o", wb_we);
        circuit->connect(br, "wb_sel_o", wb_sel);
        circuit->connect(br, "wb_stb_o", wb_stb);
        circuit->connect(br, "wb_cyc_o", wb_cyc);

        circuit->connect(mem, "clk", clk);
        circuit->connect(mem, "wb_adr_i", wb_adr);
        circuit->connect(mem, "wb_dat_i", wb_dat_o);
        circuit->connect(mem, "wb_we_i", wb_we);
        circuit->connect(mem, "wb_sel_i", wb_sel);
        circuit->connect(mem, "wb_stb_i", wb_stb);
        circuit->connect(mem, "wb_cyc_i", wb_cyc);
        circuit->connect(mem, "wb_dat_o", wb_dat_i);
        circuit->connect(mem, "wb_ack_o", wb_ack);
    }
    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        for (auto *n : {clk, awvalid, wvalid, arvalid, bready, rready})
            circuit->setWire(n->id(), (uint8_t) 0);
        circuit->setWire(wstrb->id(), (uint8_t) 0x0F);
        for (int i = 0; i < 2; ++i) cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), (uint8_t) 0);
        circuit->tick();
        circuit->setWire(clk->id(), (uint8_t) 1);
        circuit->tick();
    }
    bool bit(Net *n) { return circuit->getWire(n->id())[0] != 0; }
    uint32_t word(Net *n) {
        auto v = circuit->getWire(n->id());
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    void put(uint32_t addr, uint32_t v) { std::memcpy(mem->writableMemPtr() + addr, &v, 4); }
    uint32_t peek(uint32_t addr) { uint32_t v; std::memcpy(&v, mem->writableMemPtr() + addr, 4); return v; }

    void axiWrite(uint32_t addr, uint32_t data) {
        circuit->setWire(awaddr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), 0, 0});
        circuit->setWire(wdata->id(), {(uint8_t)(data & 0xFF), (uint8_t)((data >> 8) & 0xFF),
                                      (uint8_t)((data >> 16) & 0xFF), (uint8_t)((data >> 24) & 0xFF)});
        circuit->setWire(awvalid->id(), (uint8_t) 1);
        circuit->setWire(wvalid->id(), (uint8_t) 1);
        circuit->setWire(bready->id(), (uint8_t) 1);
        for (int i = 0; i < 30; ++i) {
            cycle();
            if (bit(awready) && bit(wready)) {
                circuit->setWire(awvalid->id(), (uint8_t) 0);
                circuit->setWire(wvalid->id(), (uint8_t) 0);
            }
        }
    }
    uint32_t axiRead(uint32_t addr) {
        circuit->setWire(araddr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), 0, 0});
        circuit->setWire(arvalid->id(), (uint8_t) 1);
        circuit->setWire(rready->id(), (uint8_t) 1);
        uint32_t got = 0;
        for (int i = 0; i < 30; ++i) {
            cycle();
            if (bit(rvalid)) {
                got = word(rdata);
                circuit->setWire(arvalid->id(), (uint8_t) 0);
            }
        }
        return got;
    }
};
} // namespace

// AW 已被接收、W 还没到时来一个读请求：读写通道之间没有顺序保证，
// 桥可以先完成读并保留已锁存的 AW，等 W 到达后再完成写
TEST(AxiBridgeTest, ReadInterleavesWithPendingWrite) {
    Fixture f;
    f.compile();
    f.put(0x30, 0);
    f.put(0x80, 0x55667788u);

    // 只送 AW，W 通道先不送。多保持几拍确保握手发生
    f.circuit->setWire(f.awaddr->id(), {0x30, 0, 0, 0});
    f.circuit->setWire(f.awvalid->id(), (uint8_t) 1);
    for (int i = 0; i < 5; ++i)
        f.cycle();
    f.circuit->setWire(f.awvalid->id(), (uint8_t) 0);

    // 写还没凑齐，此时发读；读应该正常完成
    f.circuit->setWire(f.araddr->id(), {0x80, 0, 0, 0});
    f.circuit->setWire(f.arvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.rready->id(), (uint8_t) 1);
    uint32_t got = 0;
    bool seen = false;
    for (int i = 0; i < 20; ++i) {
        f.cycle();
        if (f.bit(f.rvalid)) {
            got = f.word(f.rdata);
            seen = true;
            f.circuit->setWire(f.arvalid->id(), (uint8_t) 0);
        }
    }
    EXPECT_TRUE(seen) << "AW 未完成时读请求也应该得到响应";
    EXPECT_EQ(got, 0x55667788u);
    EXPECT_EQ(f.peek(0x30), 0u) << "W 还没到，写不能提前落到存储器";

    // W 到达后，之前锁存的 AW 应继续完成写
    f.circuit->setWire(f.wdata->id(), {0xBE, 0xBA, 0xFE, 0xCA});
    f.circuit->setWire(f.wstrb->id(), (uint8_t) 0x0F);
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.bready->id(), (uint8_t) 1);
    for (int i = 0; i < 20; ++i) {
        f.cycle();
        if (f.bit(f.wready))
            f.circuit->setWire(f.wvalid->id(), (uint8_t) 0);
    }
    EXPECT_EQ(f.peek(0x30), 0xCAFEBABEu);
}

// WSTRB 全零：不写任何字节，但事务仍然要完成并返回写响应，
// 不能因为没有字节被选中就卡在等待 Wishbone ack
TEST(AxiBridgeTest, ZeroWstrbWriteStillResponds) {
    Fixture f;
    f.compile();
    f.put(0x40, 0x11223344u);
    f.circuit->setWire(f.awaddr->id(), {0x40, 0, 0, 0});
    f.circuit->setWire(f.wdata->id(), {0xDD, 0xCC, 0xBB, 0xAA});
    f.circuit->setWire(f.wstrb->id(), (uint8_t) 0x00);
    f.circuit->setWire(f.awvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.bready->id(), (uint8_t) 0);

    bool seen_bvalid = false;
    for (int i = 0; i < 20; ++i) {
        f.cycle();
        if (f.bit(f.awready) && f.bit(f.wready)) {
            f.circuit->setWire(f.awvalid->id(), (uint8_t) 0);
            f.circuit->setWire(f.wvalid->id(), (uint8_t) 0);
        }
        if (f.bit(f.bvalid))
            seen_bvalid = true;
    }
    EXPECT_TRUE(seen_bvalid) << "全零 WSTRB 也必须返回写响应";
    EXPECT_EQ(f.peek(0x40), 0x11223344u) << "没有选中的字节不能被改写";

    f.circuit->setWire(f.bready->id(), (uint8_t) 1);
    for (int i = 0; i < 5; ++i)
        f.cycle();
    EXPECT_FALSE(f.bit(f.bvalid)) << "bready 握手后 bvalid 应撤销";
}

// 状态机只能在 clk 上升沿推进：低电平期间即使 VALID 一直为高也不能握手；
// 上升沿之后握手信号要维持整个周期，边沿触发的 AXI 主设备才能采到
TEST(AxiBridgeTest, ClockEdgeGatesHandshake) {
    Fixture f;
    f.compile();
    f.put(0x50, 0);

    f.circuit->setWire(f.clk->id(), (uint8_t) 0);
    f.circuit->setWire(f.awaddr->id(), {0x50, 0, 0, 0});
    f.circuit->setWire(f.wdata->id(), {0x11, 0x22, 0x33, 0x44});
    f.circuit->setWire(f.wstrb->id(), (uint8_t) 0x0F);
    f.circuit->setWire(f.awvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 1);
    for (int i = 0; i < 5; ++i)
        f.circuit->tick();
    EXPECT_FALSE(f.bit(f.awready)) << "没有上升沿时不能推进状态机";
    EXPECT_FALSE(f.bit(f.wready));

    // 产生上升沿，再走一个低半周期让 _c_out_ 传播到线网
    f.circuit->setWire(f.clk->id(), (uint8_t) 1);
    f.circuit->tick();
    f.circuit->setWire(f.clk->id(), (uint8_t) 0);
    f.circuit->tick();
    EXPECT_TRUE(f.bit(f.awready)) << "上升沿之后 AWREADY 应保持一个完整周期";
    EXPECT_TRUE(f.bit(f.wready));

    f.circuit->setWire(f.awvalid->id(), (uint8_t) 0);
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 0);
    f.circuit->setWire(f.bready->id(), (uint8_t) 1);
    for (int i = 0; i < 20; ++i)
        f.cycle();
    EXPECT_EQ(f.peek(0x50), 0x44332211u);
}

TEST(AxiBridgeTest, WriteReachesMemory) {
    Fixture f;
    f.compile();
    f.axiWrite(0x40, 0xDEADBEEF);
    EXPECT_EQ(f.peek(0x40), 0xDEADBEEFu) << "AXI 写应落到 Wishbone 存储器";
}

TEST(AxiBridgeTest, ReadReturnsData) {
    Fixture f;
    f.compile();
    f.put(0x80, 0x12345678);
    EXPECT_EQ(f.axiRead(0x80), 0x12345678u) << "AXI 读应返回存储器内容";
}

TEST(AxiBridgeTest, WriteDataBeforeAddress) {
    Fixture f;
    f.compile();
    f.put(0x60, 0);
    // 先只送 W 通道
    f.circuit->setWire(f.wdata->id(), {0xEF, 0xBE, 0xAD, 0xDE});
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 1);
    for (int i = 0; i < 5; ++i)
        f.cycle();
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 0);
    // 再送 AW 通道
    f.circuit->setWire(f.awaddr->id(), {0x60, 0, 0, 0});
    f.circuit->setWire(f.awvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.bready->id(), (uint8_t) 1);
    for (int i = 0; i < 20; ++i) {
        f.cycle();
        if (f.bit(f.awready))
            f.circuit->setWire(f.awvalid->id(), (uint8_t) 0);
    }
    EXPECT_EQ(f.peek(0x60), 0xDEADBEEFu) << "W 先于 AW 到达也要各自锁存，凑齐后一起发出";
}

TEST(AxiBridgeTest, WriteStrobePropagates) {
    Fixture f;
    f.compile();
    f.put(0x70, 0x11223344);
    f.circuit->setWire(f.wstrb->id(), 0x01);
    f.axiWrite(0x70, 0xAA);
    EXPECT_EQ(f.peek(0x70), 0x112233AAu) << "AXI 的 wstrb 要透传到 Wishbone 的 sel";
}

TEST(AxiBridgeTest, BvalidHeldUntilBready) {
    Fixture f;
    f.compile();
    f.put(0x90, 0);
    f.circuit->setWire(f.awaddr->id(), {0x90, 0, 0, 0});
    f.circuit->setWire(f.wdata->id(), {0x78, 0x56, 0x34, 0x12});
    f.circuit->setWire(f.wstrb->id(), 0x0F);
    f.circuit->setWire(f.awvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.wvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.bready->id(), (uint8_t) 0);
    for (int i = 0; i < 20; ++i) {
        f.cycle();
        if (f.bit(f.awready) && f.bit(f.wready)) {
            f.circuit->setWire(f.awvalid->id(), (uint8_t) 0);
            f.circuit->setWire(f.wvalid->id(), (uint8_t) 0);
        }
    }
    EXPECT_TRUE(f.bit(f.bvalid)) << "写事务完成应拉高 bvalid";
    for (int i = 0; i < 5; ++i)
        f.cycle();
    EXPECT_TRUE(f.bit(f.bvalid)) << "bready 为低时 bvalid 必须保持";
    f.circuit->setWire(f.bready->id(), (uint8_t) 1);
    for (int i = 0; i < 5; ++i)
        f.cycle();
    EXPECT_FALSE(f.bit(f.bvalid)) << "bready 握手后 bvalid 应撤销";
}

TEST(AxiBridgeTest, RvalidHeldUntilRready) {
    Fixture f;
    f.compile();
    f.put(0xA0, 0xA5A5A5A5);
    f.circuit->setWire(f.araddr->id(), {0xA0, 0, 0, 0});
    f.circuit->setWire(f.arvalid->id(), (uint8_t) 1);
    f.circuit->setWire(f.rready->id(), (uint8_t) 0);
    for (int i = 0; i < 20; ++i) {
        f.cycle();
        if (f.bit(f.arready))
            f.circuit->setWire(f.arvalid->id(), (uint8_t) 0);
    }
    EXPECT_TRUE(f.bit(f.rvalid)) << "读事务完成应拉高 rvalid";
    EXPECT_EQ(f.word(f.rdata), 0xA5A5A5A5u);
    for (int i = 0; i < 5; ++i)
        f.cycle();
    EXPECT_TRUE(f.bit(f.rvalid)) << "rready 为低时 rvalid 必须保持";
    f.circuit->setWire(f.rready->id(), (uint8_t) 1);
    for (int i = 0; i < 5; ++i)
        f.cycle();
    EXPECT_FALSE(f.bit(f.rvalid)) << "rready 握手后 rvalid 应撤销";
}
