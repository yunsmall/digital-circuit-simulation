// 计数器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(CounterTest, Increment) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 1; i <= 10; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
        EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, i & 0x0F);
    }
}

TEST(CounterTest, AsyncClear) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *clr = c.createNet("clr"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4, false, false, false, true));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "clr", clr);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 0; i < 3; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 3);
    c.setWire(clr->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
}
