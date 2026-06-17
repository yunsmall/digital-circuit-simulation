// 移位寄存器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(ShiftRegisterTest, LeftShift) {
    dsc::Circuit c;
    auto *sin = c.createNet("sin"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *sr = c.addComponent(std::make_unique<dsc::ShiftRegister>("sr", 4));
    c.connect(sr, "sin", sin);
    c.connect(sr, "clk", clk);
    c.connect(sr, "q", q);
    c.compile();
    c.init();
    // 左移：sin → LSB，每 tick 左移一位
    c.setWire(sin->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 1);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 3);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 7);
    c.setWire(sin->id(), {0, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 14);
}

TEST(ShiftRegisterTest, AsyncClear) {
    dsc::Circuit c;
    auto *sin = c.createNet("sin"), *clk = c.createNet("clk"), *clr = c.createNet("clr"), *q = c.createNet("q");
    auto *sr = c.addComponent(std::make_unique<dsc::ShiftRegister>("sr", 4, false, true));
    c.connect(sr, "sin", sin);
    c.connect(sr, "clk", clk);
    c.connect(sr, "clr", clr);
    c.connect(sr, "q", q);
    c.compile();
    c.init();
    // 移入几个1
    c.setWire(sin->id(), {1, 0});
    for (int i = 0; i < 3; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    EXPECT_NE(c.getWire(q->id())[0] & 0x0F, 0);
    // 清零
    c.setWire(clr->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0);
}
