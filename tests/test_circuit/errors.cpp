// 错误处理测试
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>

TEST(ErrorTest, UnconnectedPinIsZero) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in0);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
}

TEST(ErrorTest, InvalidGateInputs) {
    EXPECT_THROW(dsc::LogicGate("g", 0, 8, dsc::GateOp::AND), std::invalid_argument);
    EXPECT_THROW(dsc::LogicGate("g", 9, 8, dsc::GateOp::AND), std::invalid_argument);
    EXPECT_THROW(dsc::LogicGate("g", 2, 129, dsc::GateOp::AND), std::invalid_argument);
}
