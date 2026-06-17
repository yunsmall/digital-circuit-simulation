// 加法器测试
#include <dcs/Circuit.h>
#include <dcs/components/Arithmetic.h>
#include <gtest/gtest.h>

TEST(AdderTest, NoCarry) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *cin = c.createNet("cin"), *sum = c.createNet("sum"),
         *cout = c.createNet("cout");
    auto *g = c.addComponent(std::make_unique<dsc::Adder>("add", 8));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "cin", cin);
    c.connect(g, "sum", sum);
    c.connect(g, "cout", cout);
    c.compile();
    c.init();
    c.setWire(a->id(), {0x10, 0});
    c.setWire(b->id(), {0x20, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0x30);
    EXPECT_EQ(c.getWire(cout->id())[0], 0x00);
}

TEST(AdderTest, WithCarryIn) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *cin = c.createNet("cin"), *sum = c.createNet("sum"),
         *cout = c.createNet("cout");
    auto *g = c.addComponent(std::make_unique<dsc::Adder>("add", 8));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "cin", cin);
    c.connect(g, "sum", sum);
    c.connect(g, "cout", cout);
    c.compile();
    c.init();
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0x01, 0});
    c.setWire(cin->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0x01);
    EXPECT_EQ(c.getWire(cout->id())[0], 0x01);
}

TEST(AdderTest, Overflow) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *cin = c.createNet("cin"), *sum = c.createNet("sum"),
         *cout = c.createNet("cout");
    auto *g = c.addComponent(std::make_unique<dsc::Adder>("add", 4));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "cin", cin);
    c.connect(g, "sum", sum);
    c.connect(g, "cout", cout);
    c.compile();
    c.init();
    c.setWire(a->id(), {0x0F, 0});
    c.setWire(b->id(), {0x0F, 0});
    c.setWire(cin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(sum->id())[0], 0x0E);
    EXPECT_EQ(c.getWire(cout->id())[0], 0x01);
}
