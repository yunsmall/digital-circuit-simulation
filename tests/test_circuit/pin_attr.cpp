// 引脚属性测试
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(PinAttrTest, CombPinProps) {
    dsc::LogicGate g("g", 2, 8, dsc::GateOp::AND);
    EXPECT_FALSE(g.outputs()[0]->isTriState());
    EXPECT_FALSE(g.outputs()[0]->isSequential());
}

TEST(PinAttrTest, TriStatePinProps) {
    dsc::GateTSBUF g("g", 8);
    EXPECT_TRUE(g.outputs()[0]->isTriState());
    EXPECT_FALSE(g.outputs()[0]->isSequential());
}

TEST(PinAttrTest, SequentialPinProps) {
    dsc::DFlipFlop g("g", 8);
    EXPECT_FALSE(g.outputs()[0]->isTriState());
    EXPECT_TRUE(g.outputs()[0]->isSequential());
}
