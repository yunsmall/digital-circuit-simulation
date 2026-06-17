// 电路重建测试
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>

TEST(CircuitRebuildTest, Rebuild) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *in1 = c.createNet("in1"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in0);
    c.connect(g, "in1", in1);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xFF, 0});
    c.setWire(in1->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x0F);

    c.clear();
    in0 = c.createNet("in0");
    in1 = c.createNet("in1");
    out = c.createNet("out");
    g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::OR));
    c.connect(g, "in0", in0);
    c.connect(g, "in1", in1);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xF0, 0});
    c.setWire(in1->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xFF);
}
