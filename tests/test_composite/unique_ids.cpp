// 嵌套复合元件 ID 唯一性测试
#include <dcs/Circuit.h>
#include <dcs/CircuitLibrary.h>
#include <dcs/components/Composite.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>
#include <set>

TEST(CompositeTest, UniqueIds) {
    dsc::CircuitDefinition def;
    def.name = "inner_and";
    def.nets = {"ia", "ib", "iy"};
    def.components = {
            {"and", "iand", {{"inputs", "2"}, {"bit_width", "8"}}, {{"in0", "ia"}, {"in1", "ib"}, {"out", "iy"}}},
    };
    def.input_expose = {{"a", "ia"}, {"b", "ib"}};
    def.output_expose = {{"y", "iy"}};
    dsc::CircuitLibrary::instance().add(std::move(def));

    dsc::CircuitDefinition outer;
    outer.name = "outer_not_and";
    outer.nets = {"a", "b", "mid", "y"};
    outer.components = {
            {"composite", "inner", {{"definition", "inner_and"}}, {{"a", "a"}, {"b", "b"}, {"y", "mid"}}},
            {"not", "not_g", {{"bit_width", "8"}}, {{"in", "mid"}, {"out", "y"}}},
    };
    outer.input_expose = {{"a", "a"}, {"b", "b"}};
    outer.output_expose = {{"y", "y"}};
    dsc::CircuitLibrary::instance().add(std::move(outer));

    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *y = c.createNet("y");
    auto *out = c.addComponent(std::make_unique<dsc::CompositeComponent>("out", "outer_not_and"));
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
    EXPECT_EQ(c.getWire(y->id())[0], 0xF0);
}
