// 时钟发生器测试
#include <dcs/Circuit.h>
#include <dcs/components/sequential/Sequential.h>
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

// 1/1 走"每拍异或"的专用路径：长时间运行不会回绕，reset 后第一拍仍是高
TEST(ClockGenTest, PeriodTwoToggleLongRun) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk");
    auto *gen = c.addComponent(std::make_unique<dsc::ClockGen>("gen", 1, 1));
    c.connect(gen, "clk", clk);
    c.compile();
    c.init();
    for (int i = 0; i < 1000; i++) {
        c.tick();
        EXPECT_EQ(c.getWire(clk->id())[0], (i % 2 == 0) ? 1 : 0) << "tick " << i;
    }
    c.reset();
    c.tick();
    EXPECT_EQ(c.getWire(clk->id())[0], 1);
}

// 1/1 的生成代码不应该再有计数器/比较，防止以后改回通用实现
TEST(ClockGenTest, PeriodTwoHasNoCounter) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk");
    auto *gen = c.addComponent(std::make_unique<dsc::ClockGen>("gen", 1, 1));
    c.connect(gen, "clk", clk);
    c.compile();
    EXPECT_EQ(c.generateC().find("_cnt"), std::string::npos);
}
