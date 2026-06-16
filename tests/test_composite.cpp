// 复合元件测试（定义/实例分离架构）
#include <dcs/Circuit.h>
#include <dcs/CircuitLibrary.h>
#include <dcs/components/Composite.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>
#include <set>

using namespace dsc;

// 注册半加器定义
static void regHalfAdder() {
    if (CircuitLibrary::instance().exists("half_adder"))
        return;
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
}

// 注册全加器定义（用两个半加器 + OR 门）
static void regFullAdder() {
    if (CircuitLibrary::instance().exists("full_adder"))
        return;
    regHalfAdder(); // 依赖
    CircuitDefinition def;
    def.name = "full_adder";
    def.nets = {"a", "b", "cin", "s1", "c1", "c2", "sum", "cout"};
    def.components = {
            {"composite",
             "ha1",
             {{"definition", "half_adder"}},
             {{"a", "a"}, {"b", "b"}, {"sum", "s1"}, {"carry", "c1"}}},
            {"composite",
             "ha2",
             {{"definition", "half_adder"}},
             {{"a", "s1"}, {"b", "cin"}, {"sum", "sum"}, {"carry", "c2"}}},
            {"or", "or", {{"inputs", "2"}, {"bit_width", "1"}}, {{"in0", "c1"}, {"in1", "c2"}, {"out", "cout"}}},
    };
    def.input_expose = {{"a", "a"}, {"b", "b"}, {"cin", "cin"}};
    def.output_expose = {{"sum", "sum"}, {"cout", "cout"}};
    CircuitLibrary::instance().add(std::move(def));
}

// 半加器测试
TEST(CompositeTest, HalfAdder) {
    regHalfAdder();
    Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b");
    auto *sum = c.createNet("sum"), *carry = c.createNet("carry");
    auto *ha = c.addComponent(std::make_unique<CompositeComponent>("ha", "half_adder"));
    c.connect(ha, "a", a);
    c.connect(ha, "b", b);
    c.connect(ha, "sum", sum);
    c.connect(ha, "carry", carry);
    c.compile();
    c.init();

    c.setWire(a->id(), {0, 0});
    c.setWire(b->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0);
    EXPECT_EQ(c.getWire(carry->id())[0], 0);
    c.setWire(a->id(), {1, 0});
    c.setWire(b->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 1);
    c.setWire(a->id(), {1, 0});
    c.setWire(b->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0);
    EXPECT_EQ(c.getWire(carry->id())[0], 1);
}

// 嵌套全加器（两层复合展开）
TEST(CompositeTest, NestedFullAdder) {
    regFullAdder();
    Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *cin = c.createNet("cin");
    auto *sum = c.createNet("sum"), *cout = c.createNet("cout");
    auto *fa = c.addComponent(std::make_unique<CompositeComponent>("fa", "full_adder"));
    c.connect(fa, "a", a);
    c.connect(fa, "b", b);
    c.connect(fa, "cin", cin);
    c.connect(fa, "sum", sum);
    c.connect(fa, "cout", cout);
    c.compile();
    c.init();

    // 1+1+0 = 2 → sum=0, cout=1
    c.setWire(a->id(), {1, 0});
    c.setWire(b->id(), {1, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0);
    EXPECT_EQ(c.getWire(cout->id())[0], 1);

    // 1+1+1 = 3 → sum=1, cout=1
    c.setWire(cin->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 1);
    EXPECT_EQ(c.getWire(cout->id())[0], 1);
}

// 验证嵌套复合元件展开后所有元件 ID 唯一
TEST(CompositeTest, UniqueIds) {
    CircuitDefinition def;
    def.name = "inner_and";
    def.nets = {"ia", "ib", "iy"};
    def.components = {
            {"and", "iand", {{"inputs", "2"}, {"bit_width", "8"}}, {{"in0", "ia"}, {"in1", "ib"}, {"out", "iy"}}},
    };
    def.input_expose = {{"a", "ia"}, {"b", "ib"}};
    def.output_expose = {{"y", "iy"}};
    CircuitLibrary::instance().add(std::move(def));

    CircuitDefinition outer;
    outer.name = "outer_not_and";
    outer.nets = {"a", "b", "mid", "y"};
    outer.components = {
            {"composite", "inner", {{"definition", "inner_and"}}, {{"a", "a"}, {"b", "b"}, {"y", "mid"}}},
            {"not", "not_g", {{"bit_width", "8"}}, {{"in", "mid"}, {"out", "y"}}},
    };
    outer.input_expose = {{"a", "a"}, {"b", "b"}};
    outer.output_expose = {{"y", "y"}};
    CircuitLibrary::instance().add(std::move(outer));

    Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *y = c.createNet("y");
    auto *out = c.addComponent(std::make_unique<CompositeComponent>("out", "outer_not_and"));
    c.connect(out, "a", a);
    c.connect(out, "b", b);
    c.connect(out, "y", y);
    c.compile();

    std::set<int> ids;
    for (auto &comp: c.components()) {
        ASSERT_TRUE(ids.insert(comp->id()).second) << "ID 重复: " << comp->id();
        EXPECT_EQ(c.componentById(comp->id()), comp.get());
    }
    EXPECT_GE(ids.size(), 2u);

    c.init();
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0x0F, 0});
    c.tick();
    // inner AND: 0x0F → outer NOT: 0xF0
    EXPECT_EQ(c.getWire(y->id())[0], 0xF0);
}
