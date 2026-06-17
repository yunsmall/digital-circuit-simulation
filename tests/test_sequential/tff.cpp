// T 触发器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(TFlipFlopTest, Toggle) {
    dsc::Circuit c;
    auto *t = c.createNet("t"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::TFlipFlop>("tf", 4));
    c.connect(ff, "t", t);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x0F);
    c.setWire(t->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x0F);
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00);
}
