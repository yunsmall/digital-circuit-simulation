// PWM：PERIOD/DUTY/CTRL 与输出波形
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/Pwm.h>

using namespace dsc;

namespace {

struct PwmFixture {
    std::unique_ptr<Circuit> c;
    Pwm *p = nullptr;
    Net *clk, *addr, *wdata, *we, *en, *rdata, *ready, *out;

    PwmFixture() {
        c = std::make_unique<Circuit>();
        auto q = std::make_unique<Pwm>("p0");
        p = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk");
        addr = c->createNet("addr");
        wdata = c->createNet("wdata");
        we = c->createNet("we");
        en = c->createNet("en");
        rdata = c->createNet("rdata");
        ready = c->createNet("ready");
        out = c->createNet("pwm");
        for (auto [pin, net] : {std::pair{"clk", clk}, {"addr", addr}, {"wdata", wdata}, {"we", we},
                                {"en", en}, {"rdata", rdata}, {"ready", ready}, {"pwm", out}})
            c->connect(p, pin, net);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
    }

    void ticks(int n) {
        for (int i = 0; i < n; i++) c->tick();
    }
    void write(uint32_t a, uint32_t d) {
        c->setWire(addr->id(), (uint8_t) a);
        c->setWire(wdata->id(), {(uint8_t) d, (uint8_t) (d >> 8), (uint8_t) (d >> 16), (uint8_t) (d >> 24)});
        c->setWire(we->id(), 1);
        c->setWire(en->id(), 1);
        ticks(2);
        c->setWire(en->id(), 0);
        ticks(1);
    }
    uint32_t read(uint32_t a) {
        c->setWire(addr->id(), (uint8_t) a);
        c->setWire(we->id(), 0);
        c->setWire(en->id(), 1);
        ticks(3);
        auto v = c->getWire(rdata->id());
        c->setWire(en->id(), 0);
        ticks(1);
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    int level() const { return c->getWire(out->id())[0] & 1; }
    // 采样 n 拍里高电平的比例
    int countHigh(int n) {
        int hi = 0;
        for (int i = 0; i < n; i++) {
            c->tick();
            hi += level();
        }
        return hi;
    }
};

} // namespace

TEST(PwmTest, FiftyPercentDuty) {
    PwmFixture f;
    f.write(0x00, 100);
    f.write(0x04, 50);
    f.write(0x08, 1);
    int hi = f.countHigh(1000);
    EXPECT_GE(hi, 480);
    EXPECT_LE(hi, 520) << "50% 占空比偏差过大";
}

TEST(PwmTest, ZeroDutyIsAlwaysLow) {
    PwmFixture f;
    f.write(0x00, 100);
    f.write(0x04, 0);
    f.write(0x08, 1);
    EXPECT_EQ(f.countHigh(500), 0) << "DUTY=0 时必须恒低";
}

TEST(PwmTest, DutyNotLessThanPeriodIsAlwaysHigh) {
    PwmFixture f;
    f.write(0x00, 100);
    f.write(0x04, 100); // DUTY == PERIOD
    f.write(0x08, 1);
    EXPECT_EQ(f.countHigh(500), 500) << "DUTY>=PERIOD 时必须恒高";
}

TEST(PwmTest, DisabledIsAlwaysLow) {
    PwmFixture f;
    f.write(0x00, 10);
    f.write(0x04, 5);
    f.write(0x08, 0); // 不使能
    EXPECT_EQ(f.countHigh(300), 0) << "未使能时输出必须恒低";
}

TEST(PwmTest, ZeroPeriodDoesNotHang) {
    PwmFixture f;
    f.write(0x00, 0);
    f.write(0x04, 1);
    f.write(0x08, 1);
    EXPECT_EQ(f.countHigh(100), 0) << "PERIOD=0 时输出应保持低而不是除零/溢出";
}

TEST(PwmTest, RegistersReadBack) {
    PwmFixture f;
    f.write(0x00, 1234);
    f.write(0x04, 567);
    f.write(0x08, 1);
    EXPECT_EQ(f.read(0x00), 1234u);
    EXPECT_EQ(f.read(0x04), 567u);
    EXPECT_EQ(f.read(0x08), 1u);
}

TEST(PwmTest, CtrlOnlyKeepsLowBit) {
    PwmFixture f;
    f.write(0x08, 0xFFFFFFFF);
    EXPECT_EQ(f.read(0x08), 1u) << "CTRL 只有 bit0 有效";
}

TEST(PwmTest, UnknownAddressReadsZero) {
    PwmFixture f;
    EXPECT_EQ(f.read(0x7C), 0u);
}

TEST(PwmTest, SmallDutyProducesNarrowPulse) {
    PwmFixture f;
    f.write(0x00, 10);
    f.write(0x04, 1);
    f.write(0x08, 1);
    auto n = f.countHigh(100);
    EXPECT_GE(n, 1) << "占空比 1/10 不应完全无高电平";
    EXPECT_LE(n, 20);
}

TEST(PwmTest, PeriodChangeTakesEffect) {
    PwmFixture f;
    f.write(0x00, 4);
    f.write(0x04, 2);
    f.write(0x08, 1);
    auto a = f.countHigh(40);
    f.write(0x00, 16);
    f.write(0x04, 8);
    auto b = f.countHigh(160);
    EXPECT_GE(a, 10);
    EXPECT_LE(a, 30);
    EXPECT_GE(b, 60) << "新的 PERIOD/DUTY 没有生效";
    EXPECT_LE(b, 100);
}
