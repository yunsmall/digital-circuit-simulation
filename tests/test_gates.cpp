#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <format>
#include <gtest/gtest.h>

static uint8_t runGate(int in0, int in1, int num_in, int bw, auto make) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *o = c.createNet("out");
    auto *g = c.addComponent(make("g", num_in, bw));
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    if (num_in > 2)
        for (int i = 2; i < num_in; i++) {
            auto *nx = c.createNet(std::format("in{}", i));
            c.connect(g, std::format("in{}", i), nx);
        }
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(i0->id(), {static_cast<uint8_t>(in0), 0});
    c.setWire(i1->id(), {static_cast<uint8_t>(in1), 0});
    c.tick();
    return c.getWire(o->id())[0];
}

#define GATE_TEST(name, cls, in0, in1, expected)                                                                       \
    TEST(GatesTest, name) {                                                                                            \
        EXPECT_EQ(runGate(in0, in1, 2, 8, [](auto &&...a) { return std::make_unique<cls>(a...); }),                    \
                  static_cast<uint8_t>(expected));                                                                     \
    }

GATE_TEST(AND, dsc::GateAND, 0xFF, 0x0F, 0x0F)
GATE_TEST(OR, dsc::GateOR, 0xF0, 0x0F, 0xFF)
GATE_TEST(NAND, dsc::GateNAND, 0xFF, 0x0F, ~0x0F)
GATE_TEST(NOR, dsc::GateNOR, 0xF0, 0x0F, ~(0xF0 | 0x0F))
GATE_TEST(XOR, dsc::GateXOR, 0xF0, 0x0F, 0xF0 ^ 0x0F)
GATE_TEST(XNOR, dsc::GateXNOR, 0xF0, 0x0F, ~(0xF0 ^ 0x0F))

TEST(GatesTest, NOT) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateNOT>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xF0);
}
TEST(GatesTest, BUF) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateBUF>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAB);
}
TEST(GatesTest, TSBUF) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *oe = c.createNet("oe"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateTSBUF>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "oe", oe);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(oe->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAB);
}
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
