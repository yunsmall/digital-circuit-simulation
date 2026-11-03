// 时钟分频器测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

TEST(ClockDividerTest, DivBy2) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::ClockDivider>("d", 2));
    c.connect(g, "clk", clk);
    c.connect(g, "out", out);
    c.compile();
    c.init();

    std::vector<uint8_t> v1(1, 1), v0(1, 0);

    // init 后先跑一个 tick 触发时序逻辑（counter=0, d2=1 → out=1）
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);

    // 第一个上升沿 → counter=1, out=0
    c.setWire(clk->id(), v1);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
    c.setWire(clk->id(), v0);
    c.tick();

    // 第二个上升沿 → counter=0, out=1
    c.setWire(clk->id(), v1);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

// 奇数分频无法做到 50%：divisor=3 时高 1 拍、低 2 拍（33%），文档按这个行为写
TEST(ClockDividerTest, DivBy3DutyIsFloorOverCeil) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::ClockDivider>("d", 3));
    c.connect(g, "clk", clk);
    c.connect(g, "out", out);
    c.compile();
    c.init();

    std::vector<uint8_t> v1(1, 1), v0(1, 0);
    auto rising = [&]() {
        c.setWire(clk->id(), v1);
        c.tick();
        c.setWire(clk->id(), v0);
        c.tick();
    };

    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1) << "counter=0 时输出高";
    rising(); // counter=1
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
    rising(); // counter=2
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
    rising(); // 回绕到 0
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

TEST(ClockDividerTest, DivBy4) {    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::ClockDivider>("d", 4));
    c.connect(g, "clk", clk);
    c.connect(g, "out", out);
    c.compile();
    c.init();

    std::vector<uint8_t> v1(1, 1), v0(1, 0);

    auto rising = [&]() {
        c.setWire(clk->id(), v1);
        c.tick();
        c.setWire(clk->id(), v0);
        c.tick();
    };

    // counter=0, 0<2 → out=1
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
    // counter=1, out=1
    rising();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
    // counter=2, out=0
    rising();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
    // counter=3, out=0
    rising();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
    // counter=0, out=1
    rising();
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}
