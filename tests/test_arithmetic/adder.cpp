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

// ---- Adder（有符号）----
// 加法位模式两种模式相同，差别在 cout：有符号模式应报溢出（同号相加得异号）
TEST(ArithmeticTest, AdderSignedOverflow) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *s = c.createNet("s");
    auto *co = c.createNet("co");
    auto *ad = c.addComponent(std::make_unique<dsc::Adder>("ad", 8, true));
    c.connect(ad, "a", a);
    c.connect(ad, "b", b);
    c.connect(ad, "sum", s);
    c.connect(ad, "cout", co);
    c.compile();
    c.init();

    auto sv = [&] { return (uint32_t) c.getWire(s->id())[0]; };
    auto cv = [&] { return (uint32_t) c.getWire(co->id())[0]; };

    // 127 + 1 = -128，有符号溢出
    c.setWire(a->id(), {127, 0});
    c.setWire(b->id(), {1, 0});
    c.tick();
    EXPECT_EQ(sv(), 0x80u);
    EXPECT_EQ(cv(), 1u) << "127+1 应报有符号溢出";

    // (-128) + (-1) = 127，有符号溢出
    c.setWire(a->id(), {0x80, 0});
    c.setWire(b->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(sv(), 0x7Fu);
    EXPECT_EQ(cv(), 1u) << "-128+-1 应报有符号溢出";

    // 1 + 2 = 3，无溢出（虽然无符号进位角度看也没进位）
    c.setWire(a->id(), {1, 0});
    c.setWire(b->id(), {2, 0});
    c.tick();
    EXPECT_EQ(sv(), 3u);
    EXPECT_EQ(cv(), 0u);

    // (-1) + (-1) = -2，同号相加得同号，不溢出
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(sv(), 0xFEu);
    EXPECT_EQ(cv(), 0u) << "同号相加得同号不算溢出";
}

