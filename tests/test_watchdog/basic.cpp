// Watchdog：超时、喂狗、关闭重开
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/Watchdog.h>

using namespace dsc;

namespace {

struct WdFixture {
    std::unique_ptr<Circuit> c;
    Watchdog *w = nullptr;
    Net *clk, *addr, *wdata, *we, *en, *rdata, *ready, *irq;

    WdFixture() {
        c = std::make_unique<Circuit>();
        auto q = std::make_unique<Watchdog>("w0");
        w = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk");
        addr = c->createNet("addr");
        wdata = c->createNet("wdata");
        we = c->createNet("we");
        en = c->createNet("en");
        rdata = c->createNet("rdata");
        ready = c->createNet("ready");
        irq = c->createNet("irq");
        for (auto [pin, net] : {std::pair{"clk", clk}, {"addr", addr}, {"wdata", wdata}, {"we", we},
                                {"en", en}, {"rdata", rdata}, {"ready", ready}, {"irq", irq}})
            c->connect(w, pin, net);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
    }

    void ticks(int n) {
        for (int i = 0; i < n; i++) c->tick();
    }
    void write(uint32_t a, uint32_t d) {
        c->setWire(addr->id(), {(uint8_t) a});
        c->setWire(wdata->id(), {(uint8_t) d, (uint8_t) (d >> 8), (uint8_t) (d >> 16), (uint8_t) (d >> 24)});
        c->setWire(we->id(), {1});
        c->setWire(en->id(), {1});
        ticks(2);
        c->setWire(en->id(), {0});
        ticks(1);
    }
    uint32_t read(uint32_t a) {
        c->setWire(addr->id(), {(uint8_t) a});
        c->setWire(we->id(), {0});
        c->setWire(en->id(), {1});
        ticks(3);
        auto v = c->getWire(rdata->id());
        c->setWire(en->id(), {0});
        ticks(1);
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    int irqLevel() const { return c->getWire(irq->id())[0] & 1; }
};

} // namespace

TEST(WatchdogTest, FiresAfterTimeout) {
    WdFixture f;
    f.write(0x04, 50);  // 50 拍超时
    f.write(0x00, 1);   // 使能
    f.ticks(20);
    EXPECT_EQ(f.irqLevel(), 0) << "还没到超时时间就拉中断了";
    f.ticks(40);
    EXPECT_EQ(f.irqLevel(), 1) << "超时后 irq 没有拉高";
    EXPECT_EQ(f.read(0x0C), 1u) << "STATUS 没有反映超时";
}

TEST(WatchdogTest, FeedPreventsTimeout) {
    WdFixture f;
    f.write(0x04, 100);
    f.write(0x00, 1);
    for (int i = 0; i < 10; i++) {
        f.ticks(50);
        f.write(0x08, 1); // 喂狗
    }
    EXPECT_EQ(f.irqLevel(), 0) << "按时喂狗不应该触发超时";
}

TEST(WatchdogTest, FeedClearsFiredIrq) {
    WdFixture f;
    f.write(0x04, 10);
    f.write(0x00, 1);
    f.ticks(30);
    ASSERT_EQ(f.irqLevel(), 1);
    f.write(0x08, 1);
    f.ticks(3);
    EXPECT_EQ(f.irqLevel(), 0) << "喂狗后中断必须撤销";
    EXPECT_EQ(f.read(0x0C), 0u);
}

TEST(WatchdogTest, ZeroTimeoutNeverFires) {
    WdFixture f;
    f.write(0x00, 1); // 使能但 TIMEOUT=0
    f.ticks(500);
    EXPECT_EQ(f.irqLevel(), 0) << "TIMEOUT=0 时不应触发";
}

TEST(WatchdogTest, DisableClearsState) {
    WdFixture f;
    f.write(0x04, 10);
    f.write(0x00, 1);
    f.ticks(30);
    ASSERT_EQ(f.irqLevel(), 1);
    f.write(0x00, 0); // 关闭
    f.ticks(3);
    EXPECT_EQ(f.irqLevel(), 0) << "关闭看门狗必须撤销中断";
    f.write(0x00, 1); // 重新使能后应重新计时
    f.ticks(5);
    EXPECT_EQ(f.irqLevel(), 0) << "重新使能后应从头计时";
}

TEST(WatchdogTest, StatusRegisterReadsFired) {
    WdFixture f;
    f.write(0x04, 3);
    f.write(0x00, 1);
    f.ticks(5);
    EXPECT_EQ(f.read(0x0C), 1u);
}

TEST(WatchdogTest, UnknownAddressReadsZero) {
    WdFixture f;
    EXPECT_EQ(f.read(0x7C), 0u);
}

TEST(WatchdogTest, TimeoutZeroDoesNotFire) {
    WdFixture f;
    f.write(0x00, 1);
    f.ticks(10);
    EXPECT_EQ(f.irqLevel(), 0) << "TIMEOUT=0 不应触发";
    f.write(0x04, 4);
    f.ticks(6);
    EXPECT_EQ(f.irqLevel(), 1) << "设置超时后应重新开始计时";
}

TEST(WatchdogTest, DisableThenReenableRestarts) {
    WdFixture f;
    f.write(0x04, 3);
    f.write(0x00, 1);
    f.ticks(5);
    EXPECT_EQ(f.irqLevel(), 1);
    f.write(0x00, 0);
    f.ticks(2);
    EXPECT_EQ(f.irqLevel(), 0);
    // 用更长的超时验证计数确实从头开始：write 内部只走了 4 拍，远不到 10
    f.write(0x04, 10);
    f.write(0x00, 1);
    f.ticks(2);
    EXPECT_EQ(f.irqLevel(), 0) << "重新使能后计数应从头开始";
}
