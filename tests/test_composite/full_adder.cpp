// 嵌套全加器复合元件测试
#include <dcs/Circuit.h>
#include <gtest/gtest.h>
#include "common.h"

TEST(CompositeTest, NestedFullAdder) {
    regFullAdder();
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *cin = c.createNet("cin");
    auto *sum = c.createNet("sum"), *cout = c.createNet("cout");
    auto *fa = c.addComponent(std::make_unique<dsc::CompositeComponent>("fa", "full_adder"));
    c.connect(fa, "a", a);
    c.connect(fa, "b", b);
    c.connect(fa, "cin", cin);
    c.connect(fa, "sum", sum);
    c.connect(fa, "cout", cout);
    c.compile();
    c.init();
    c.setWire(a->id(), {1, 0});
    c.setWire(b->id(), {1, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0);
    EXPECT_EQ(c.getWire(cout->id())[0], 1);
    c.setWire(cin->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 1);
    EXPECT_EQ(c.getWire(cout->id())[0], 1);
}
