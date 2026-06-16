// JSON 序列化往返测试
#include <gtest/gtest.h>
#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/Arithmetic.h"
#include "dcs/components/Composite.h"
#include "dcs/components/Dll.h"
#include "dcs/components/Gates.h"
#include "dcs/components/Memory.h"
#include "dcs/components/Misc.h"
#include "dcs/components/Sequential.h"
using namespace dsc;

// 所有元件类型的工厂注册已集中在 src/ComponentFactory.cpp 中，
// 由 ComponentFactory 自身保证链接，无需 ForceLinkAll 等黑魔法。

// 基本门电路 JSON 往返测试
TEST(JsonRoundTrip, BasicGates) {
    Circuit c;
    auto *n0 = c.createNet("in0");
    auto *n1 = c.createNet("in1");
    auto *n2 = c.createNet("out_and");
    auto *n3 = c.createNet("out_or");

    auto *g1 = c.addComponent(std::make_unique<GateAND>("g1", 2, 8));
    c.connect(g1, "in0", n0);
    c.connect(g1, "in1", n1);
    c.connect(g1, "out", n2);

    auto *g2 = c.addComponent(std::make_unique<GateOR>("g2", 2, 8));
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
    EXPECT_NE(c2->findNet("in1"), nullptr);
    EXPECT_NE(c2->findNet("out_and"), nullptr);
    EXPECT_NE(c2->findNet("out_or"), nullptr);

    EXPECT_EQ(c2->componentCount(), 2);
    auto *cg1 = c2->componentById(0);
    auto *cg2 = c2->componentById(1);
    ASSERT_NE(cg1, nullptr);
    ASSERT_NE(cg2, nullptr);
    EXPECT_EQ(cg1->typeName(), "and");
    EXPECT_EQ(cg2->typeName(), "or");

    EXPECT_EQ(cg1->inputs()[0]->net()->name(), "in0");
    EXPECT_EQ(cg1->inputs()[1]->net()->name(), "in1");
    EXPECT_EQ(cg1->outputs()[0]->net()->name(), "out_and");

    c2->compile();
    c2->init();
    c2->setWire(c2->findNet("in0")->id(), {0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    c2->setWire(c2->findNet("in1")->id(), {0x0F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    c2->tick();
    auto out_and = c2->getWire(c2->findNet("out_and")->id());
    auto out_or = c2->getWire(c2->findNet("out_or")->id());
    EXPECT_EQ(out_and[0], 0x0F);
    EXPECT_EQ(out_or[0], 0xFF);
}

// 时序电路 JSON 往返
TEST(JsonRoundTrip, Sequential) {
    Circuit c;
    auto *d = c.createNet("d");
    auto *clk = c.createNet("clk");
    auto *q = c.createNet("q");

    auto *dff = c.addComponent(std::make_unique<DFlipFlop>("dff1", 8));
    c.connect(dff, "d", d);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", q);

    std::string json = c.exportJson();

    std::string error;
    auto c2 = Circuit::fromJson(json, error);
    ASSERT_TRUE(c2) << error;
    EXPECT_EQ(c2->netCount(), 3);
    EXPECT_EQ(c2->componentCount(), 1);
    EXPECT_EQ(c2->componentById(0)->typeName(), "dff");
}

// 从工厂创建所有类型
TEST(JsonRoundTrip, FromFactory) {
    auto &f = ComponentFactory::instance();

    // 验证所有注册类型都可以创建
    // 注意: 工厂注册名（用户可见）可能与 Component::typeName()（C 函数名用）不同
    struct {
        const char *factory_name;
        const char *expected_type_name;
        std::unordered_map<std::string, std::string> params;
    } types[] = {
            {"and", "and", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"or", "or", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"nand", "nand", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"nor", "nor", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"xor", "xor", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"xnor", "xnor", {{"inputs", "2"}, {"bit_width", "8"}}},
            {"not", "not", {{"bit_width", "8"}}},
            {"buf", "buf", {{"bit_width", "8"}}},
            {"tsbuf", "tsbuf", {{"bit_width", "8"}}},
            {"dff", "dff", {{"bit_width", "8"}}},
            {"tff", "tff", {{"bit_width", "8"}}},
            {"jkff", "jkff", {{"bit_width", "8"}}},
            {"register", "reg", {{"bit_width", "8"}}},
            {"latch", "latch", {{"bit_width", "8"}}},
            {"counter", "cnt", {{"bit_width", "8"}}},
            {"shift", "sr", {{"length", "8"}}},
            {"mux", "mux", {{"n_selects", "1"}, {"bit_width", "8"}}},
            {"adder", "adder", {{"bit_width", "8"}}},
            {"comparator", "cmp_eq", {{"bit_width", "8"}, {"op", "eq"}}},
            {"decoder", "decoder", {{"n_selects", "2"}}},
            {"constant", "const", {{"bit_width", "8"}, {"value", "42"}}},
            {"switch", "switch", {{"bit_width", "8"}}},
            {"merge", "merge", {{"num_bits", "8"}}},
            {"split", "split", {{"num_bits", "8"}}},
            {"memory", "memory", {{"addr_width", "4"}, {"data_width", "8"}}},
            {"encoder", "encoder", {{"n_selects", "2"}}},
            {"sub", "sub", {{"bit_width", "8"}}},
            {"mul", "mul", {{"bit_width", "8"}}},
            {"rom", "rom", {{"addr_width", "4"}, {"data_width", "8"}}},
    };

    for (auto &t: types) {
        auto comp = f.create(t.factory_name, "test", t.params);
        EXPECT_NE(comp, nullptr) << "无法创建类型: " << t.factory_name;
        if (comp) {
            EXPECT_EQ(comp->typeName(), t.expected_type_name) << "typeName 不匹配: " << t.factory_name;
            auto ep = comp->exportParams();
            EXPECT_FALSE(ep.empty()) << t.factory_name << " 未导出参数";
        }
    }
}

// 复合元件 JSON 往返（定义/实例分离）
TEST(JsonRoundTrip, Composite) {
    // 1. 注册共享定义
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

    // 2. 用两个实例构建电路
    Circuit c;
    auto *a0 = c.createNet("a0");
    auto *b0 = c.createNet("b0");
    auto *s0 = c.createNet("sum0");
    auto *c0 = c.createNet("carry0");
    auto *a1 = c.createNet("a1");
    auto *b1 = c.createNet("b1");
    auto *s1 = c.createNet("sum1");
    auto *c1out = c.createNet("carry1");

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

    // 3. 导出入 JSON 再重建
    std::string json = c.exportJson();
    EXPECT_FALSE(json.empty());

    std::string error;
    auto c2 = Circuit::fromJson(json, error);
    ASSERT_TRUE(c2 != nullptr) << error;

    // 4. 编译仿真
    c2->compile();
    c2->init();
    // ha0: 1+0 => sum=1
    c2->setWire(c2->findNet("a0")->id(), {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    c2->setWire(c2->findNet("b0")->id(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    c2->tick();
    EXPECT_EQ(c2->getWire(c2->findNet("sum0")->id())[0], 1);
    EXPECT_EQ(c2->getWire(c2->findNet("carry0")->id())[0], 0);
}
