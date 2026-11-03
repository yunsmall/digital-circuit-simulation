// 环路检测测试
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

TEST(CircuitCheckTest, CombinationalLoopDetected) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 8, true));
    c.connect(g, "in", w);
    c.connect(g, "out", w);
    auto err = c.check();
    EXPECT_NE(err.code, dsc::ErrorCode::OK);
    // 闭环起点只应出现两次（进入环、绕回起点），不能在结尾再多拼一个
    EXPECT_EQ(err.message.find("w -> w -> w"), std::string::npos) << err.message;
    EXPECT_NE(err.message.find("w -> w"), std::string::npos) << err.message;
}

TEST(CircuitCheckTest, NoLoopWithSequential) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *d = c.createNet("d"), *q = c.createNet("q");
    auto *not_g = c.addComponent(std::make_unique<dsc::UnaryGate>("not", 8, true));
    c.connect(not_g, "in", q);
    c.connect(not_g, "out", d);
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    EXPECT_EQ(c.check().code, dsc::ErrorCode::OK);
}

TEST(CircuitCheckTest, MultipleDriversError) {
    dsc::Circuit c;
    auto *w = c.createNet("w"), *in = c.createNet("in");
    auto *g1 = c.addComponent(std::make_unique<dsc::UnaryGate>("b1", 8, false));
    auto *g2 = c.addComponent(std::make_unique<dsc::UnaryGate>("b2", 8, false));
    c.connect(g1, "in", in);
    c.connect(g2, "in", in);
    c.connect(g1, "out", w);
    EXPECT_EQ(c.connect(g2, "out", w).code, dsc::ErrorCode::MULTIPLE_DRIVERS);
}
