// 除法器测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Arithmetic.h>
#include <gtest/gtest.h>

TEST(DividerTest, UnsignedDiv) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *q = c.createNet("quot"), *r = c.createNet("rem");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", 8));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();
    // 100 / 7 = 14 余 2
    c.setWire(a->id(), {100, 0});
    c.setWire(b->id(), {7, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 14);
    EXPECT_EQ(c.getWire(r->id())[0], 2);
}

TEST(DividerTest, ExactDiv) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *q = c.createNet("quot"), *r = c.createNet("rem");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", 8));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();
    c.setWire(a->id(), {128, 0});
    c.setWire(b->id(), {16, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 8);
    EXPECT_EQ(c.getWire(r->id())[0], 0);
}

TEST(DividerTest, DivByOne) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *q = c.createNet("quot"), *r = c.createNet("rem");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", 8));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();
    c.setWire(a->id(), {0xAB, 0});
    c.setWire(b->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    EXPECT_EQ(c.getWire(r->id())[0], 0);
}

TEST(DividerTest, DivByZero) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *q = c.createNet("quot"), *r = c.createNet("rem");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", 8));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();
    c.setWire(a->id(), {0xAB, 0});
    c.setWire(b->id(), {0, 0});
    c.tick();
    // 除零保护：商和余数都应为 0
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    EXPECT_EQ(c.getWire(r->id())[0], 0);
}

TEST(DividerTest, Wide16) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *q = c.createNet("quot"), *r = c.createNet("rem");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", 16));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();
    // 65535 / 256 = 255 余 255
    std::vector<uint8_t> v65535 = {0xFF, 0xFF, 0};
    std::vector<uint8_t> v256 = {0, 1, 0};
    c.setWire(a->id(), v65535);
    c.setWire(b->id(), v256);
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 255);
    EXPECT_EQ(c.getWire(q->id())[1], 0);
    EXPECT_EQ(c.getWire(r->id())[0], 255);
}

// ---- Divider（有符号）----
// 旧实现直接 (int64_t)_a，窄类型是无符号的，-128/1 会算成 128/1
TEST(DividerTest, SignedNegative) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *q = c.createNet("q");
    auto *r = c.createNet("r");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", 8, true));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();

    auto qv = [&] { return (uint32_t) c.getWire(q->id())[0]; };
    auto rv = [&] { return (uint32_t) c.getWire(r->id())[0]; };

    // (-128) / 1 = -128 = 0x80, 余 0
    c.setWire(a->id(), {0x80, 0});
    c.setWire(b->id(), {1, 0});
    c.tick();
    EXPECT_EQ(qv(), 0x80u) << "-128/1 商应为 -128";
    EXPECT_EQ(rv(), 0u);

    // (-7) / 2 = -3, 余 -1
    c.setWire(a->id(), {0xF9, 0});
    c.setWire(b->id(), {2, 0});
    c.tick();
    EXPECT_EQ(qv(), 0xFDu) << "-7/2 商应为 -3";
    EXPECT_EQ(rv(), 0xFFu) << "-7%2 余应为 -1";

    // 7 / (-2) = -3, 余 1
    c.setWire(a->id(), {7, 0});
    c.setWire(b->id(), {0xFE, 0});
    c.tick();
    EXPECT_EQ(qv(), 0xFDu) << "7/-2 商应为 -3";
    EXPECT_EQ(rv(), 1u) << "7%-2 余应为 1";
}

