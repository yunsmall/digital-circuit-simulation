// D 触发器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(DFlipFlopTest, RisingEdge) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    c.setWire(d->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(clk->id(), {0, 0});
    c.setWire(d->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x55);
}

TEST(DFlipFlopTest, AsyncReset) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *rst = c.createNet("rst"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8, false, true));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "rst", rst);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(d->id(), {0xAB, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(rst->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00);
    c.setWire(rst->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
}
