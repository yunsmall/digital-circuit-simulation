// 环路检测测试
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(CircuitCheckTest, CombinationalLoopDetected) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 8, true));
    c.connect(g, "in", w);
    c.connect(g, "out", w);
    EXPECT_NE(c.check().code, dsc::ErrorCode::OK);
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
    EXPECT_THROW(c.connect(g2, "out", w), std::runtime_error);
}
