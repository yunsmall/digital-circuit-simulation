// 时钟发生器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(ClockGenTest, EqualDuty) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk");
    auto *gen = c.addComponent(std::make_unique<dsc::ClockGen>("gen", 1, 1));
    c.connect(gen, "clk", clk);
    c.compile();
    c.init();
    // 周期=2: 高低交替
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 1);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 0);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 1);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 0);
}

TEST(ClockGenTest, UnequalDuty) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk");
    auto *gen = c.addComponent(std::make_unique<dsc::ClockGen>("gen", 2, 3));
    c.connect(gen, "clk", clk);
    c.compile();
    c.init();
    // 高 2 tick，低 3 tick，周期=5
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 1);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 1);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 0);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 0);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 0);
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 1);
}
