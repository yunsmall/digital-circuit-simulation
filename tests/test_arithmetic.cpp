#include <dcs/Circuit.h>
#include <dcs/components/Arithmetic.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>

// ============================================================
// Mux — 多路选择器
// ============================================================
TEST(MuxTest, TwoToOne) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Mux>("m", 1, 8));
    c.connect(g, "sel0", s0);
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "out", o);
    c.compile();
    c.init();

    c.setWire(i0->id(), {0xAB, 0});
    c.setWire(i1->id(), {0xCD, 0});

    c.setWire(s0->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xAB);

    c.setWire(s0->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xCD);
}

TEST(MuxTest, FourToOne) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *s1 = c.createNet("s1"), *i0 = c.createNet("in0"), *i1 = c.createNet("in1"),
         *i2 = c.createNet("in2"), *i3 = c.createNet("in3"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Mux>("m", 2, 4));
    c.connect(g, "sel0", s0);
    c.connect(g, "sel1", s1);
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "in2", i2);
    c.connect(g, "in3", i3);
    c.connect(g, "out", o);
    c.compile();
    c.init();

    c.setWire(i0->id(), {0x0A, 0});
    c.setWire(i1->id(), {0x0B, 0});
    c.setWire(i2->id(), {0x0C, 0});
    c.setWire(i3->id(), {0x0D, 0});

    c.setWire(s0->id(), {0, 0});
    c.setWire(s1->id(), {1, 0});
    c.tick(); // sel=2
    EXPECT_EQ(c.getWire(o->id())[0], 0x0C);

    c.setWire(s0->id(), {1, 0});
    c.setWire(s1->id(), {1, 0});
    c.tick(); // sel=3
    EXPECT_EQ(c.getWire(o->id())[0], 0x0D);
}

// ============================================================
// Adder — 加法器
// ============================================================
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
    EXPECT_EQ(c.getWire(sum->id())[0], 0x01); // 0xFF + 1 + 1 = 0x101 → sum=0x01
    EXPECT_EQ(c.getWire(cout->id())[0], 0x01); // carry
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
    EXPECT_EQ(c.getWire(sum->id())[0], 0x0E); // 15+15=30 → low 4 bits = 14
    EXPECT_EQ(c.getWire(cout->id())[0], 0x01);
}

// ============================================================
// Comparator — 比较器
// ============================================================
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

// ============================================================
// Decoder — 译码器
// ============================================================
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

    // sel=0 → out0=1
    c.setWire(s0->id(), {0, 0});
    c.setWire(s1->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o0->id())[0], 1);
    EXPECT_EQ(c.getWire(o1->id())[0], 0);
    EXPECT_EQ(c.getWire(o2->id())[0], 0);
    EXPECT_EQ(c.getWire(o3->id())[0], 0);

    // sel=3 → out3=1
    c.setWire(s0->id(), {1, 0});
    c.setWire(s1->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o0->id())[0], 0);
    EXPECT_EQ(c.getWire(o1->id())[0], 0);
    EXPECT_EQ(c.getWire(o2->id())[0], 0);
    EXPECT_EQ(c.getWire(o3->id())[0], 1);
}

TEST(DecoderTest, ThreeToEight) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *s1 = c.createNet("s1"), *s2 = c.createNet("s2");
    std::vector<dsc::Net *> outs;
    for (int i = 0; i < 8; i++)
        outs.push_back(c.createNet("o" + std::to_string(i)));

    auto *g = c.addComponent(std::make_unique<dsc::Decoder>("dec", 3));
    c.connect(g, "sel0", s0);
    c.connect(g, "sel1", s1);
    c.connect(g, "sel2", s2);
    for (int i = 0; i < 8; i++)
        c.connect(g, std::format("out{}", i), outs[i]);
    c.compile();
    c.init();

    // sel=5 → out5=1
    c.setWire(s0->id(), {1, 0});
    c.setWire(s1->id(), {0, 0});
    c.setWire(s2->id(), {1, 0});
    c.tick();
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(c.getWire(outs[i]->id())[0], i == 5 ? 1 : 0) << "out" << i;
}

// Encoder 测试
TEST(ArithmeticTest, Encoder) {
    dsc::Circuit c;
    auto *enc = c.addComponent(std::make_unique<dsc::Encoder>("enc", 3));
    std::vector<dsc::Net *> ins;
    for (int i = 0; i < 8; i++)
        ins.push_back(c.createNet(std::format("in{}", i)));
    dsc::Net *out0 = c.createNet("out0");
    dsc::Net *out1 = c.createNet("out1");
    dsc::Net *out2 = c.createNet("out2");
    for (int i = 0; i < 8; i++)
        c.connect(enc, std::format("in{}", i), ins[i]);
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

// Subtractor 测试
TEST(ArithmeticTest, Subtractor) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *bin = c.createNet("bin");
    auto *diff = c.createNet("diff");
    auto *bout = c.createNet("bout");
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
    c.setWire(bin->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(diff->id())[0], 254);
    EXPECT_EQ(c.getWire(bout->id())[0], 1);
}

// Multiplier 测试
TEST(ArithmeticTest, Multiplier) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *b = c.createNet("b");
    auto *prod = c.createNet("prod");
    auto *mul = c.addComponent(std::make_unique<dsc::Multiplier>("mul", 8));
    c.connect(mul, "a", a);
    c.connect(mul, "b", b);
    c.connect(mul, "prod", prod);
    c.compile();
    c.init();

    c.setWire(a->id(), {5, 0});
    c.setWire(b->id(), {3, 0});
    c.tick();
    EXPECT_EQ(c.getWire(prod->id())[0], 15);
    EXPECT_EQ(c.getWire(prod->id())[1], 0);

    c.setWire(a->id(), {255, 0});
    c.setWire(b->id(), {255, 0});
    c.tick();
    EXPECT_EQ(c.getWire(prod->id())[0], 0x01);
    EXPECT_EQ(c.getWire(prod->id())[1], 0xFE);
}
