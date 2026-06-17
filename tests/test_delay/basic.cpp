// 延时线基本测试
#include <dcs/Circuit.h>
#include <dcs/components/DelayLine.h>
#include <gtest/gtest.h>

TEST(DelayLineTest, Depth1_BasicDelay) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 8, 1));
    c.connect(dl, "in", in);
    c.connect(dl, "clk", clk);
    c.connect(dl, "out", out);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(out->id())[0], 0);

    c.setWire(in->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(in->id(), {0x55, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAB);

    c.setWire(in->id(), {0x12, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x55);
}
