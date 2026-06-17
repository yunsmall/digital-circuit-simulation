// 基本多输入门测试：AND, OR, NAND, NOR, XOR, XNOR
#include "common.h"

GATE_TEST(AND, dsc::GateAND, 0xFF, 0x0F, 0x0F)
GATE_TEST(OR, dsc::GateOR, 0xF0, 0x0F, 0xFF)
GATE_TEST(NAND, dsc::GateNAND, 0xFF, 0x0F, ~0x0F)
GATE_TEST(NOR, dsc::GateNOR, 0xF0, 0x0F, ~(0xF0 | 0x0F))
GATE_TEST(XOR, dsc::GateXOR, 0xF0, 0x0F, 0xF0 ^ 0x0F)
GATE_TEST(XNOR, dsc::GateXNOR, 0xF0, 0x0F, ~(0xF0 ^ 0x0F))

TEST(GatesTest, MultiBit4_AND) {
    EXPECT_EQ(runGate(0x0E, 0x07, 2, 4, [](auto &&...a) { return std::make_unique<dsc::GateAND>(a...); }), 0x0E & 0x07);
}

TEST(GatesTest, MultiInput3_AND) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *i2 = c.createNet("in2"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateAND>("g", 3, 8));
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
