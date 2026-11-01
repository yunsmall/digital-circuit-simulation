// I2C 主机：空闲电平、起始条件、地址数据位序、NACK 检测
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/I2C.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    I2C *i2c = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *dat_o = nullptr;
    Net *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
    Net *sda_in = nullptr, *ack = nullptr, *scl = nullptr, *sda_out = nullptr, *sda_oe = nullptr;

    explicit Fixture(int clkdiv = 4) {
        auto c = std::make_unique<I2C>("i2c0", clkdiv);
        i2c = c.get();
        circuit->addComponent(std::move(c));
        clk = circuit->createNet("clk");
        adr = circuit->createNet("adr");
        dat_i = circuit->createNet("dat_i");
        dat_o = circuit->createNet("dat_o");
        we = circuit->createNet("we");
        sel = circuit->createNet("sel");
        stb = circuit->createNet("stb");
        cyc = circuit->createNet("cyc");
        sda_in = circuit->createNet("sda_in");
        ack = circuit->createNet("ack");
        scl = circuit->createNet("scl");
        sda_out = circuit->createNet("sda_out");
        sda_oe = circuit->createNet("sda_oe");
        circuit->connect(i2c, "clk", clk);
        circuit->connect(i2c, "wb_adr_i", adr);
        circuit->connect(i2c, "wb_dat_i", dat_i);
        circuit->connect(i2c, "wb_we_i", we);
        circuit->connect(i2c, "wb_sel_i", sel);
        circuit->connect(i2c, "wb_stb_i", stb);
        circuit->connect(i2c, "wb_cyc_i", cyc);
        circuit->connect(i2c, "sda_in", sda_in);
        circuit->connect(i2c, "wb_dat_o", dat_o);
        circuit->connect(i2c, "wb_ack_o", ack);
        circuit->connect(i2c, "scl", scl);
        circuit->connect(i2c, "sda_out", sda_out);
        circuit->connect(i2c, "sda_oe", sda_oe);
    }
    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), 0);
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        circuit->setWire(we->id(), 0);
        circuit->setWire(sda_in->id(), 1); // 外部上拉
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

TEST(I2cTest, IdleBusIsHigh) {
    Fixture f;
    f.compile();
    EXPECT_TRUE(f.bit(f.scl)) << "空闲 SCL 应为高";
    EXPECT_FALSE(f.bit(f.sda_oe)) << "空闲时不驱动 SDA（开漏释放）";
    EXPECT_TRUE(f.bit(f.sda_out)) << "开漏释放时输出视为高";
    EXPECT_EQ(f.read(0x08) & 1u, 0u) << "空闲时应不忙";
}

TEST(I2cTest, StartConditionAndTransfer) {
    Fixture f(4);
    f.compile();
    f.write(0x10, 0x50); // 从机地址 0x50
    f.write(0x00, 0xA5); // 数据
    f.write(0x0C, 1);    // 发起事务
    EXPECT_EQ(f.read(0x08) & 1u, 1u) << "发起后应处于忙状态";
    bool sawStart = false;
    for (int i = 0; i < 60; ++i) {
        f.cycle();
        if (f.bit(f.scl) && !f.bit(f.sda_out) && f.bit(f.sda_oe))
            sawStart = true; // SCL 高时把 SDA 拉低即为起始条件
    }
    EXPECT_TRUE(sawStart) << "应出现起始条件";
}

TEST(I2cTest, NackReportedWhenSlaveSilent) {
    Fixture f(4);
    f.compile();
    f.write(0x10, 0x50);
    f.write(0x00, 0xA5);
    f.write(0x0C, 1);
    // sda_in 一直保持高 = 从机不应答
    for (int i = 0; i < 200; ++i)
        f.cycle();
    EXPECT_NE(f.read(0x08) & 4u, 0u) << "从机不应答应置 NACK";
    EXPECT_NE(f.read(0x08) & 2u, 0u) << "事务应结束";
    EXPECT_EQ(f.read(0x08) & 1u, 0u) << "结束后应回到空闲";
}

TEST(I2cTest, SlaveAckCompletesWithoutNack) {
    Fixture f(4);
    f.compile();
    f.write(0x10, 0x50);
    f.write(0x00, 0xA5);
    f.circuit->setWire(f.sda_in->id(), (uint8_t) 0); // 从机全程拉低 = 每个 ACK 位都应答
    f.write(0x0C, 1);
    for (int i = 0; i < 200; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x08) & 4u, 0u) << "从机 ACK 时不应报 NACK";
    EXPECT_NE(f.read(0x08) & 2u, 0u) << "事务应正常结束";
    EXPECT_EQ(f.read(0x08) & 1u, 0u) << "结束后应回到空闲";
}

TEST(I2cTest, RestartAfterDone) {
    Fixture f(4);
    f.compile();
    f.write(0x10, 0x50);
    f.write(0x00, 0xA5);
    f.write(0x0C, 1);
    for (int i = 0; i < 200; ++i)
        f.cycle();
    ASSERT_NE(f.read(0x08) & 2u, 0u) << "第一次事务应结束";
    f.write(0x00, 0x5A);
    f.write(0x0C, 1); // 再次启动
    EXPECT_EQ(f.read(0x08) & 1u, 1u) << "完成后必须能再次启动，否则控制器只能用一次";
    EXPECT_EQ(f.read(0x08) & 2u, 0u) << "重新启动应清掉完成标志";
}

TEST(I2cTest, ClkdivLockedWhileBusy) {
    Fixture f(4);
    f.compile();
    f.write(0x10, 0x50);
    f.write(0x00, 0xA5);
    f.write(0x0C, 1);
    f.write(0x14, 8); // 事务进行中改分频
    EXPECT_EQ(f.read(0x14), 4u) << "事务进行中不能改分频，否则位时序会被打乱";
}

TEST(I2cTest, UnknownRegisterReadsZero) {
    Fixture f;
    f.compile();
    EXPECT_EQ(f.read(0x70), 0u);
}

TEST(I2cTest, ZeroClkdivStillReturnsToIdle) {
    Fixture f(0); // 构造函数会把 0 钳到 1
    f.compile();
    f.write(0x10, 0x50);
    f.write(0x00, 0xA5);
    f.write(0x0C, 1);
    for (int i = 0; i < 200; ++i)
        f.cycle();
    EXPECT_EQ(f.read(0x08) & 1u, 0u) << "clkdiv=0 不应让状态机卡在忙状态";
}
