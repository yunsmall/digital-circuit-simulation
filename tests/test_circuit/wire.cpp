// 线网位宽测试
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>

TEST(WireTest, BitWidthDeterminedByMaxPin) {
    dsc::Circuit c;
    auto *w = c.createNet("w"), *in = c.createNet("in");
    auto *g4 = c.addComponent(std::make_unique<dsc::GateBUF>("b4", 4));
    auto *g8 = c.addComponent(std::make_unique<dsc::GateBUF>("b8", 8));
    c.connect(g4, "in", in);
    c.connect(g4, "out", w);
    EXPECT_EQ(w->bit_width(), 4);
    c.connect(g8, "in", w);
    EXPECT_EQ(w->bit_width(), 8);
}
