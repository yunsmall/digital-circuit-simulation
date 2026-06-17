// JSON 序列化：复合元件
#include "dcs/components/Composite.h"
#include <gtest/gtest.h>
#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
using namespace dsc;

TEST(JsonRoundTrip, Composite) {
    CircuitDefinition def;
    def.name = "half_adder";
    def.nets = {"a", "b", "sum", "carry"};
    def.components = {
            {"xor", "xor", {{"inputs", "2"}, {"bit_width", "1"}}, {{"in0", "a"}, {"in1", "b"}, {"out", "sum"}}},
            {"and", "and", {{"inputs", "2"}, {"bit_width", "1"}}, {{"in0", "a"}, {"in1", "b"}, {"out", "carry"}}},
    };
    def.input_expose = {{"a", "a"}, {"b", "b"}};
    def.output_expose = {{"sum", "sum"}, {"carry", "carry"}};
    CircuitLibrary::instance().add(std::move(def));

    Circuit c;
    auto *a0 = c.createNet("a0"), *b0 = c.createNet("b0"), *s0 = c.createNet("sum0"), *c0 = c.createNet("carry0");
    auto *a1 = c.createNet("a1"), *b1 = c.createNet("b1"), *s1 = c.createNet("sum1"), *c1out = c.createNet("carry1");
    auto *ha0 = c.addComponent(std::make_unique<CompositeComponent>("ha0", "half_adder"));
    c.connect(ha0, "a", a0);
    c.connect(ha0, "b", b0);
    c.connect(ha0, "sum", s0);
    c.connect(ha0, "carry", c0);
    auto *ha1 = c.addComponent(std::make_unique<CompositeComponent>("ha1", "half_adder"));
    c.connect(ha1, "a", a1);
    c.connect(ha1, "b", b1);
    c.connect(ha1, "sum", s1);
    c.connect(ha1, "carry", c1out);

    std::string json = c.exportJson();
    EXPECT_FALSE(json.empty());
    std::string error;
    auto c2 = Circuit::fromJson(json, error);
    ASSERT_TRUE(c2 != nullptr) << error;
    c2->compile();
    c2->init();
    c2->setWire(c2->findNet("a0")->id(), {1, 0});
    c2->setWire(c2->findNet("b0")->id(), {0, 0});
    c2->tick();
    EXPECT_EQ(c2->getWire(c2->findNet("sum0")->id())[0], 1);
}
