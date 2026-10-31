// 比较器、编码器、减法器、乘法器、译码器测试
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Arithmetic.h>
#include <format>
#include <gtest/gtest.h>

// ---- Comparator ----
TEST(ComparatorTest, Equal) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Comparator>("cmp", 8, dsc::CmpOp::EQ));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), {0xAB, 0});
    c.setWire(b->id(), {0xAB, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), {0xAB, 0});
    c.setWire(b->id(), {0xCD, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(ComparatorTest, LessThan) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Comparator>("cmp", 8, dsc::CmpOp::LT));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), {0x10, 0});
    c.setWire(b->id(), {0x20, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0x10, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(ComparatorTest, GreaterEqual) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Comparator>("cmp", 8, dsc::CmpOp::GE));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), {0x10, 0});
    c.setWire(b->id(), {0x20, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

// ---- Decoder ----
TEST(DecoderTest, TwoToFour) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *s1 = c.createNet("s1"), *o0 = c.createNet("o0"), *o1 = c.createNet("o1"),
         *o2 = c.createNet("o2"), *o3 = c.createNet("o3");
    auto *g = c.addComponent(std::make_unique<dsc::Decoder>("dec", 2));
    c.connect(g, "sel0", s0);
    c.connect(g, "sel1", s1);
    c.connect(g, "out0", o0);
    c.connect(g, "out1", o1);
    c.connect(g, "out2", o2);
    c.connect(g, "out3", o3);
    c.compile();
    c.init();
    c.setWire(s0->id(), {0, 0});
    c.setWire(s1->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o0->id())[0], 1);
    EXPECT_EQ(c.getWire(o1->id())[0], 0);
    EXPECT_EQ(c.getWire(o2->id())[0], 0);
    EXPECT_EQ(c.getWire(o3->id())[0], 0);
    c.setWire(s0->id(), {1, 0});
    c.setWire(s1->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o3->id())[0], 1);
}

TEST(DecoderTest, ThreeToEight) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *s1 = c.createNet("s1"), *s2 = c.createNet("s2");
    dsc::Net *outs[8];
    for (int i = 0; i < 8; i++)
        outs[i] = c.createNet("o" + std::to_string(i));
    auto *g = c.addComponent(std::make_unique<dsc::Decoder>("dec", 3));
    c.connect(g, "sel0", s0);
    c.connect(g, "sel1", s1);
    c.connect(g, "sel2", s2);
    for (int i = 0; i < 8; i++)
        c.connect(g, std::format("out{}", i), outs[i]);
    c.compile();
    c.init();
    c.setWire(s0->id(), {1, 0});
    c.setWire(s1->id(), {0, 0});
    c.setWire(s2->id(), {1, 0});
    c.tick();
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(c.getWire(outs[i]->id())[0], i == 5 ? 1 : 0) << "out" << i;
}

// ---- Encoder ----
TEST(ArithmeticTest, Encoder) {
    dsc::Circuit c;
    auto *enc = c.addComponent(std::make_unique<dsc::Encoder>("enc", 3));
    dsc::Net *ins[8], *out0 = c.createNet("out0"), *out1 = c.createNet("out1"), *out2 = c.createNet("out2");
    for (int i = 0; i < 8; i++) {
        ins[i] = c.createNet(std::format("in{}", i));
        c.connect(enc, std::format("in{}", i), ins[i]);
    }
    c.connect(enc, "out0", out0);
    c.connect(enc, "out1", out1);
    c.connect(enc, "out2", out2);
    c.compile();
    c.init();
    for (int i = 0; i < 8; i++)
        c.setWire(ins[i]->id(), {i == 5 ? (uint8_t) 1 : (uint8_t) 0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out0->id())[0], 1);
    EXPECT_EQ(c.getWire(out1->id())[0], 0);
    EXPECT_EQ(c.getWire(out2->id())[0], 1);
}

// ---- Subtractor ----
TEST(ArithmeticTest, Subtractor) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *bin = c.createNet("bin"), *diff = c.createNet("diff"),
         *bout = c.createNet("bout");
    auto *sub = c.addComponent(std::make_unique<dsc::Subtractor>("sub", 8));
    c.connect(sub, "a", a);
    c.connect(sub, "b", b);
    c.connect(sub, "bin", bin);
    c.connect(sub, "diff", diff);
    c.connect(sub, "bout", bout);
    c.compile();
    c.init();
    c.setWire(a->id(), {5, 0});
    c.setWire(b->id(), {3, 0});
    c.setWire(bin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(diff->id())[0], 2);
    EXPECT_EQ(c.getWire(bout->id())[0], 0);
    c.setWire(a->id(), {3, 0});
    c.setWire(b->id(), {5, 0});
    c.tick();
    EXPECT_EQ(c.getWire(diff->id())[0], 254);
    EXPECT_EQ(c.getWire(bout->id())[0], 1);
}

// ---- Multiplier ----
TEST(ArithmeticTest, Multiplier) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *lo = c.createNet("lo");
    auto *hi = c.createNet("hi");
    auto *mul = c.addComponent(std::make_unique<dsc::Multiplier>("mul", 8));
    c.connect(mul, "a", a);
    c.connect(mul, "b", b);
    c.connect(mul, "prod_lo", lo);
    c.connect(mul, "prod_hi", hi);
    c.compile();
    c.init();
    // 5 × 3 = 15: lo=15, hi=0
    c.setWire(a->id(), {5, 0});
    c.setWire(b->id(), {3, 0});
    c.tick();
    EXPECT_EQ(c.getWire(lo->id())[0], 15);
    EXPECT_EQ(c.getWire(hi->id())[0], 0);
    // 255 × 255 = 65025 = 0xFE01: lo=1, hi=0xFE
    c.setWire(a->id(), {255, 0});
    c.setWire(b->id(), {255, 0});
    c.tick();
    EXPECT_EQ(c.getWire(lo->id())[0], 0x01);
    EXPECT_EQ(c.getWire(hi->id())[0], 0xFE);
}

// ---- Multiplier（有符号）----
// 旧实现直接 (int64_t)_a，而 _a 是无符号类型，得到的是零扩展，
// 负数会被当成大正数、符号位判断失效。这里专门覆盖负数输入。
TEST(ArithmeticTest, MultiplierSignedNegative) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *lo = c.createNet("lo");
    auto *hi = c.createNet("hi");
    auto *mul = c.addComponent(std::make_unique<dsc::Multiplier>("mul", 8, true));
    c.connect(mul, "a", a);
    c.connect(mul, "b", b);
    c.connect(mul, "prod_lo", lo);
    c.connect(mul, "prod_hi", hi);
    c.compile();
    c.init();

    auto lo_v = [&] { return (uint32_t) c.getWire(lo->id())[0]; };
    auto hi_v = [&] { return (uint32_t) c.getWire(hi->id())[0]; };

    // (-2) × 3 = -6 = 0xFFFA
    c.setWire(a->id(), {0xFE, 0});
    c.setWire(b->id(), {3, 0});
    c.tick();
    EXPECT_EQ(lo_v(), 0xFAu) << "-2 * 3 的低字节";
    EXPECT_EQ(hi_v(), 0xFFu) << "-2 * 3 的高字节应为负数符号";

    // (-2) × (-3) = 6
    c.setWire(a->id(), {0xFE, 0});
    c.setWire(b->id(), {0xFD, 0});
    c.tick();
    EXPECT_EQ(lo_v(), 6u) << "负负得正";
    EXPECT_EQ(hi_v(), 0u);

    // (-128) × 1 = -128 = 0xFF80（最负的输入）
    c.setWire(a->id(), {0x80, 0});
    c.setWire(b->id(), {1, 0});
    c.tick();
    EXPECT_EQ(lo_v(), 0x80u);
    EXPECT_EQ(hi_v(), 0xFFu);
}


// ---- Comparator（有符号）----
// 旧实现直接把无符号窄类型转 int64_t，-1 会被当成 255
TEST(ArithmeticTest, ComparatorSignedNegative) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *o = c.createNet("o");
    auto *cmp = c.addComponent(std::make_unique<dsc::Comparator>("cmp", 8, dsc::CmpOp::LT, true));
    c.connect(cmp, "a", a);
    c.connect(cmp, "b", b);
    c.connect(cmp, "out", o);
    c.compile();
    c.init();

    auto ov = [&] { return (uint32_t) c.getWire(o->id())[0]; };

    // (-1) < 1 应成立
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {1, 0});
    c.tick();
    EXPECT_EQ(ov(), 1u) << "-1 应小于 1";

    // (-128) < (-1) 应成立
    c.setWire(a->id(), {0x80, 0});
    c.setWire(b->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(ov(), 1u) << "-128 应小于 -1";

    // 127 < (-128) 不应成立
    c.setWire(a->id(), {127, 0});
    c.setWire(b->id(), {0x80, 0});
    c.tick();
    EXPECT_EQ(ov(), 0u) << "127 不应小于 -128";
}

