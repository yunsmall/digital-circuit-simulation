// 半加器复合元件测试
#include <dcs/Circuit.h>
#include <gtest/gtest.h>
#include "common.h"

TEST(CompositeTest, HalfAdder) {
    regHalfAdder();
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b");
    auto *sum = c.createNet("sum"), *carry = c.createNet("carry");
    auto *ha = c.addComponent(std::make_unique<dsc::CompositeComponent>("ha", "half_adder"));
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
