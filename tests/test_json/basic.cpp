// JSON 序列化：基本门电路
#include <gtest/gtest.h>
#include "dcs/Circuit.h"
#include "dcs/components/Gates.h"
using namespace dsc;

TEST(JsonRoundTrip, BasicGates) {
    Circuit c;
    auto *n0 = c.createNet("in0"), *n1 = c.createNet("in1"), *n2 = c.createNet("out_and"), *n3 = c.createNet("out_or");
    auto *g1 = c.addComponent(std::make_unique<LogicGate>("g1", 2, 8, GateOp::AND));
    c.connect(g1, "in0", n0);
    c.connect(g1, "in1", n1);
    c.connect(g1, "out", n2);
    auto *g2 = c.addComponent(std::make_unique<LogicGate>("g2", 2, 8, GateOp::OR));
    c.connect(g2, "in0", n0);
    c.connect(g2, "in1", n1);
    c.connect(g2, "out", n3);

    std::string json = c.exportJson();
    EXPECT_FALSE(json.empty());

    std::string error;
    auto c2 = Circuit::fromJson(json, error);
    ASSERT_TRUE(c2 != nullptr) << error;
    EXPECT_EQ(c2->netCount(), 4);
    EXPECT_NE(c2->findNet("in0"), nullptr);
    EXPECT_EQ(c2->componentCount(), 2);
    EXPECT_EQ(c2->componentById(0)->typeName(), "and");
    EXPECT_EQ(c2->componentById(1)->typeName(), "or");

    c2->compile();
    c2->init();
    c2->setWire(c2->findNet("in0")->id(), {0xFF, 0});
    c2->setWire(c2->findNet("in1")->id(), {0x0F, 0});
    c2->tick();
    EXPECT_EQ(c2->getWire(c2->findNet("out_and")->id())[0], 0x0F);
    EXPECT_EQ(c2->getWire(c2->findNet("out_or")->id())[0], 0xFF);
}
