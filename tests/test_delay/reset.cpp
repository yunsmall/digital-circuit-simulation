// 延时线复位测试
#include <dcs/Circuit.h>
#include <dcs/components/DelayLine.h>
#include <gtest/gtest.h>

TEST(DelayLineTest, ResetClearsBuffer) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 8, 2));
    c.connect(dl, "in", in);
    c.connect(dl, "clk", clk);
    c.connect(dl, "out", out);
    c.compile();
    c.init();

    c.setWire(in->id(), {0xAB, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(in->id(), {0xCD, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();

    c.reset();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(in->id(), {0xEF, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
}
