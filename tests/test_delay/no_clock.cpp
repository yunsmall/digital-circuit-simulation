// 无时钟模式延时线测试
#include <dcs/Circuit.h>
#include <dcs/components/DelayLine.h>
#include <gtest/gtest.h>

TEST(DelayLineTest, NoClockMode) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 8, 3, false));
    c.connect(dl, "in", in);
    c.connect(dl, "out", out);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(out->id())[0], 0);

    c.setWire(in->id(), {10, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0);
    c.setWire(in->id(), {20, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0);
    c.setWire(in->id(), {30, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0);
    c.setWire(in->id(), {40, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 10);
    c.setWire(in->id(), {50, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 20);
}
