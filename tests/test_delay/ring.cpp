// 延时线振荡器测试
#include <dcs/Circuit.h>
#include <dcs/components/DelayLine.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>

TEST(DelayLineTest, RingOscillatorWithNOT) {
    dsc::Circuit c;
    auto *n1 = c.createNet("n1"), *n2 = c.createNet("n2");
    auto *inv = c.addComponent(std::make_unique<dsc::GateNOT>("inv", 1));
    c.connect(inv, "in", n1);
    c.connect(inv, "out", n2);
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 1, 1));
    c.connect(dl, "in", n2);
    c.connect(dl, "clk", c.createNet("clk"));
    c.connect(dl, "out", n1);

    EXPECT_EQ(c.check().code, dsc::ErrorCode::OK);
    c.compile();
    c.init();
    c.setWire(2, {0, 0});
    c.tick();
    c.setWire(2, {1, 0});
    c.tick();
    auto v1 = c.getWire(n1->id())[0] & 1;
    auto v2 = c.getWire(n2->id())[0] & 1;
    EXPECT_EQ(v2, v1 ^ 1);
}
