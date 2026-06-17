#pragma once
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <format>
#include <gtest/gtest.h>

// 通用门测试辅助
inline uint8_t runGate(int in0, int in1, int num_in, int bw, auto make) {
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

#define GATE_TEST_OP(name, op, in0, in1, expected)                                                                         \
    TEST(GatesTest, name) {                                                                                                \
        EXPECT_EQ(runGate(in0, in1, 2, 8,                                                                                  \
                          [](const std::string &n, int ni, int bw) {                                                       \
                              return std::make_unique<dsc::LogicGate>(n, ni, bw, dsc::GateOp::op);                         \
                          }),                                                                                              \
                  static_cast<uint8_t>(expected));                                                                         \
    }
