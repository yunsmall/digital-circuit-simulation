// 基本多输入门测试：AND, OR, NAND, NOR, XOR, XNOR
#include "common.h"

GATE_TEST_OP(AND, AND, 0xFF, 0x0F, 0x0F)
GATE_TEST_OP(OR, OR, 0xF0, 0x0F, 0xFF)
GATE_TEST_OP(NAND, NAND, 0xFF, 0x0F, ~0x0F)
GATE_TEST_OP(NOR, NOR, 0xF0, 0x0F, ~(0xF0 | 0x0F))
GATE_TEST_OP(XOR, XOR, 0xF0, 0x0F, 0xF0 ^ 0x0F)
GATE_TEST_OP(XNOR, XNOR, 0xF0, 0x0F, ~(0xF0 ^ 0x0F))

TEST(GatesTest, MultiBit4_AND) {
    EXPECT_EQ(runGate(0x0E, 0x07, 2, 4,
                      [](const std::string &n, int ni, int bw) {
                          return std::make_unique<dsc::LogicGate>(n, ni, bw, dsc::GateOp::AND);
                      }),
              0x0E & 0x07);
}

TEST(GatesTest, MultiInput3_AND) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *i2 = c.createNet("in2"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 3, 8, dsc::GateOp::AND));
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "in2", i2);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(i0->id(), {0xFF, 0});
    c.setWire(i1->id(), {0x0F, 0});
    c.setWire(i2->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xFF & 0x0F & 0x55);
}
