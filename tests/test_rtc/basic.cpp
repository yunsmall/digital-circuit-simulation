// RTC：goldfish 风格时间寄存器 + 闹钟中断（全部逻辑在 JIT 里）
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/RTC.h>

using namespace dsc;

namespace {

struct RtcFixture {
    std::unique_ptr<Circuit> c;
    RTC *rtc = nullptr;
    Net *clk, *addr, *wdata, *we, *en, *rdata, *ready, *irq;

    explicit RtcFixture(uint32_t ns_per_tick = 100) {
        c = std::make_unique<Circuit>();
        auto p = std::make_unique<RTC>("rtc0", ns_per_tick);
        rtc = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        addr = c->createNet("addr");
        wdata = c->createNet("wdata");
        we = c->createNet("we");
        en = c->createNet("en");
        rdata = c->createNet("rdata");
        ready = c->createNet("ready");
        irq = c->createNet("irq");
        c->connect(rtc, "clk", clk);
        c->connect(rtc, "addr", addr);
        c->connect(rtc, "wdata", wdata);
        c->connect(rtc, "we", we);
        c->connect(rtc, "en", en);
        c->connect(rtc, "rdata", rdata);
        c->connect(rtc, "ready", ready);
        c->connect(rtc, "irq", irq);
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
        ticks(3); // seq 输出写 _c_out 后下一拍才传播到线网
        auto v = c->getWire(rdata->id());
        c->setWire(en->id(), 0);
        ticks(1);
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    int irqLevel() const { return c->getWire(irq->id())[0] & 1; }
};

} // namespace

TEST(RtcTest, TimeAdvances) {
    RtcFixture f(100);
    const uint32_t t1 = f.read(0x00);
    f.ticks(1000);
    const uint32_t t2 = f.read(0x00);
    EXPECT_GT(t2, t1) << "时间寄存器没有推进";
    // 1000 拍 × 100ns = 100us，允许读取本身消耗的少量拍
    EXPECT_GE(t2 - t1, 100000u - 20000u);
}

TEST(RtcTest, AlarmHighHalfWritable) {
    RtcFixture f(100);
    f.write(0x0C, 0x00000005);
    EXPECT_EQ(f.read(0x0C), 5u) << "ALARM_HIGH 写入没有生效";
}

TEST(RtcTest, AlarmRaisesIrq) {
    RtcFixture f(100);
    const uint32_t now = f.read(0x00);
    f.write(0x08, now + 500); // 闹钟 500ns 后
    EXPECT_EQ(f.read(0x08), now + 500) << "闹钟低半没有写进去";
    f.write(0x10, 1);         // 使能
    EXPECT_EQ(f.read(0x10), 1u) << "IRQ_ENABLED 没有写进去";
    f.ticks(20);              // 2000ns，早该到期
    EXPECT_EQ(f.irqLevel(), 1) << "闹钟到期后 irq 没有拉高";
    f.write(0x14, 1); // CLEAR_ALARM
    f.ticks(4);
    EXPECT_EQ(f.irqLevel(), 0) << "CLEAR_ALARM 没有清掉中断";
}

TEST(RtcTest, ReadyFollowsEnable) {
    RtcFixture f(100);
    f.c->setWire(f.en->id(), 1);
    f.ticks(3);
    EXPECT_EQ(f.c->getWire(f.ready->id())[0] & 1, 1) << "en 有效时 ready 必须为高";
    f.c->setWire(f.en->id(), 0);
    f.ticks(3);
    EXPECT_EQ(f.c->getWire(f.ready->id())[0] & 1, 0);
}

TEST(RtcTest, ClearResetsAlarmAndIrqEnable) {
    RtcFixture f;
    f.write(0x08, 0); // ALARM_LO
    f.write(0x0C, 0); // ALARM_HI
    f.write(0x10, 1); // IRQ_EN
    f.ticks(2);
    EXPECT_EQ(f.irqLevel(), 1) << "闹钟已过应拉中断";
    f.write(0x14, 1); // CLEAR
    EXPECT_EQ(f.irqLevel(), 0);
    EXPECT_EQ(f.read(0x10), 0u);
    EXPECT_EQ(f.read(0x08), 0u);
}

TEST(RtcTest, AlarmInFutureDoesNotFire) {
    RtcFixture f;
    uint32_t lo = f.read(0x00), hi = f.read(0x04);
    uint64_t now = ((uint64_t) hi << 32) | lo;
    f.write(0x08, (uint32_t) (now + 1000000));
    f.write(0x0C, (uint32_t) ((now + 1000000) >> 32));
    f.write(0x10, 1);
    f.ticks(2);
    EXPECT_EQ(f.irqLevel(), 0) << "未来的闹钟不应触发";
}

TEST(RtcTest, UnknownAddressReadsZero) {
    RtcFixture f;
    EXPECT_EQ(f.read(0x7C), 0u);
}

TEST(RtcTest, IrqDisabledIgnoresAlarm) {
    RtcFixture f;
    f.write(0x08, 0);
    f.write(0x0C, 0);
    f.ticks(2);
    EXPECT_EQ(f.irqLevel(), 0) << "IRQ_EN=0 时不应拉中断";
}

TEST(RtcTest, CloneKeepsBaseNs) {
    RTC original("rtc", 100);
    original.setBaseNs(1700000000000000000ull);
    auto copy = original.clone("rtc2");
    auto *rtc2 = static_cast<RTC *>(copy.get());
    ASSERT_NE(rtc2, nullptr);
    EXPECT_EQ(rtc2->baseNs(), 1700000000000000000ull) << "时间基准是配置，clone 必须带上";
}
