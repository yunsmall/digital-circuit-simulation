// SPI 主机：启动传输、位时序、回环收发
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/SPI.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    SPI *spi = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *dat_o = nullptr;
    Net *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
    Net *miso = nullptr, *ack = nullptr, *sclk = nullptr, *mosi = nullptr, *cs = nullptr;

    explicit Fixture(int clkdiv = 2) {
        auto s = std::make_unique<SPI>("spi0", 1, clkdiv);
        spi = s.get();
        circuit->addComponent(std::move(s));
        clk = circuit->createNet("clk");
        adr = circuit->createNet("adr");
        dat_i = circuit->createNet("dat_i");
        dat_o = circuit->createNet("dat_o");
        we = circuit->createNet("we");
        sel = circuit->createNet("sel");
        stb = circuit->createNet("stb");
        cyc = circuit->createNet("cyc");
        miso = circuit->createNet("miso");
        ack = circuit->createNet("ack");
        sclk = circuit->createNet("sclk");
        mosi = circuit->createNet("mosi");
        cs = circuit->createNet("cs");
        circuit->connect(spi, "clk", clk);
        circuit->connect(spi, "wb_adr_i", adr);
        circuit->connect(spi, "wb_dat_i", dat_i);
        circuit->connect(spi, "wb_we_i", we);
        circuit->connect(spi, "wb_sel_i", sel);
        circuit->connect(spi, "wb_stb_i", stb);
        circuit->connect(spi, "wb_cyc_i", cyc);
        circuit->connect(spi, "miso", miso);
        circuit->connect(spi, "wb_dat_o", dat_o);
        circuit->connect(spi, "wb_ack_o", ack);
        circuit->connect(spi, "sclk", sclk);
        circuit->connect(spi, "mosi", mosi);
        circuit->connect(spi, "cs", cs);
    }
    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), 0);
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        circuit->setWire(we->id(), 0);
        circuit->setWire(miso->id(), 0);
        for (int i = 0; i < 2; ++i) cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), 0);
        circuit->tick();
        circuit->setWire(clk->id(), 1);
        circuit->tick();
    }
    void write(uint32_t addr, uint32_t v) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(dat_i->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), 0, 0});
        circuit->setWire(we->id(), 1);
        circuit->setWire(sel->id(), 0x0F);
        circuit->setWire(stb->id(), 1);
        circuit->setWire(cyc->id(), 1);
        for (int i = 0; i < 4; ++i) cycle();
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; ++i) cycle();
    }
    uint32_t read(uint32_t addr) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(we->id(), 0);
        circuit->setWire(sel->id(), 0x0F);
        circuit->setWire(stb->id(), 1);
        circuit->setWire(cyc->id(), 1);
        for (int i = 0; i < 6; ++i) cycle();
        auto v = circuit->getWire(dat_o->id());
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; ++i) cycle();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    bool bit(Net *n) { return circuit->getWire(n->id())[0] != 0; }
};
} // namespace

TEST(SpiTest, IdleLinesAndCs) {
    Fixture f;
    f.compile();
    EXPECT_TRUE(f.bit(f.cs)) << "空闲时片选应为高";
    EXPECT_FALSE(f.bit(f.sclk)) << "模式 0 空闲时钟为低";
    EXPECT_EQ(f.read(0x08) & 1u, 0u) << "空闲时应不忙";
}

TEST(SpiTest, TransferProducesClockAndDone) {
    Fixture f(16); // 每 16 个时钟周期翻转一次 SCLK
    f.compile();
    f.write(0x00, 0xA5);
    f.write(0x0C, 1); // 启动
    EXPECT_FALSE(f.bit(f.cs)) << "传输期间片选拉低";
    int edges = 0;
    bool last = f.bit(f.sclk);
    for (int i = 0; i < 600; ++i) {
        f.cycle();
        bool cur = f.bit(f.sclk);
        if (cur != last)
            edges++;
        last = cur;
    }
    EXPECT_GE(edges, 16) << "8 位传输应产生至少 16 个时钟边沿";
    EXPECT_EQ(f.read(0x08) & 2u, 2u) << "传输结束应置完成标志";
    EXPECT_TRUE(f.bit(f.cs)) << "结束后片选回到高";
}

TEST(SpiTest, LoopbackReceivesShiftedByte) {
    Fixture f(16);
    f.compile();
    // 把 mosi 接到 miso，形成回环；改完连线必须重新编译，否则 JIT 还是旧连接
    f.circuit->disconnect(f.spi, "miso");
    f.circuit->connect(f.spi, "miso", f.mosi);
    f.compile();
    f.write(0x00, 0x3C);
    f.write(0x0C, 1);
    for (int i = 0; i < 260; ++i)
        f.cycle();
    EXPECT_NE(f.read(0x08) & 2u, 0u) << "应完成一次传输";
    EXPECT_EQ(f.read(0x04), 0x3Cu) << "回环收到的字节";
}

TEST(SpiTest, WriteDuringTransferIgnored) {
    Fixture f(16);
    f.compile();
    f.circuit->disconnect(f.spi, "miso");
    f.circuit->connect(f.spi, "miso", f.mosi);
    f.compile();
    f.write(0x00, 0x3C);
    f.write(0x0C, 1);
    for (int i = 0; i < 40; ++i)
        f.cycle();
    f.write(0x00, 0xFF); // 传输中改写 TXDATA
    for (int i = 0; i < 260; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x04), 0x3Cu) << "传输中改写 TXDATA 不应破坏当前字节";
}

TEST(SpiTest, RestartAfterDone) {
    Fixture f(16);
    f.compile();
    f.circuit->disconnect(f.spi, "miso");
    f.circuit->connect(f.spi, "miso", f.mosi);
    f.compile();
    f.write(0x00, 0x11);
    f.write(0x0C, 1);
    for (int i = 0; i < 260; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x04), 0x11u) << "第一次传输";
    // 读 RXDATA 会清 done，之后必须还能再启动一次
    f.write(0x00, 0x22);
    f.write(0x0C, 1);
    for (int i = 0; i < 260; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x04), 0x22u) << "完成后应能再次启动";
}

TEST(SpiTest, ClkdivWritableWhenIdle) {
    Fixture f(16);
    f.compile();
    f.write(0x10, 4);
    EXPECT_EQ(f.read(0x10), 4u);
    f.write(0x10, 0); // 0 分频无意义，忽略
    EXPECT_EQ(f.read(0x10), 4u) << "0 分频应被忽略";
}

TEST(SpiTest, UnknownRegisterReadsZero) {
    Fixture f;
    f.compile();
    // addr 只解码低 5 位，0x7E → 0x1E，不对应任何寄存器
    EXPECT_EQ(f.read(0x7E), 0u);
}

TEST(SpiTest, ZeroClkdivStillCompletes) {
    Fixture f(0); // 构造函数会把 0 钳到 1
    f.compile();
    f.write(0x00, 0x5A);
    f.write(0x0C, 1);
    for (int i = 0; i < 60; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x08) & 2u, 2u) << "clkdiv=0 钳到最小值后仍应完成传输";
}
