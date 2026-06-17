// 多级延时和边沿测试
#include <dcs/Circuit.h>
#include <dcs/components/DelayLine.h>
#include <gtest/gtest.h>

TEST(DelayLineTest, Depth3_MultiTickDelay) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 8, 3));
    c.connect(dl, "in", in);
    c.connect(dl, "clk", clk);
    c.connect(dl, "out", out);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(out->id())[0], 0);

    int inputs[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) {
        c.setWire(in->id(), {(uint8_t) inputs[i], 0});
        c.setWire(clk->id(), {0, 0});
        c.tick();
        c.setWire(clk->id(), {1, 0});
        c.tick();
        EXPECT_EQ(c.getWire(out->id())[0], 0);
    }
    c.setWire(in->id(), {40, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 10);

    c.setWire(in->id(), {50, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 20);
}

TEST(DelayLineTest, NoChangeOnFallingEdge) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 8, 2));
    c.connect(dl, "in", in);
    c.connect(dl, "clk", clk);
    c.connect(dl, "out", out);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(in->id(), {0xAB, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(in->id(), {0xCD, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(in->id(), {0x55, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAB);
}
