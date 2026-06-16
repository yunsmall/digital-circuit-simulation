#include <dcs/Circuit.h>
#include <dcs/components/Arithmetic.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

// 测试各关键位宽的 AND 门：类型边界（8/16/32/64）及边界附近
static void gateBitWidthTest(int bw) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateAND>("g", 2, bw));
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "out", o);
    c.compile();
    c.init();

    // 写入全 1（bw 位）
    std::vector<uint8_t> ones(16, 0xFF), pat(16, 0);
    for (int i = (bw + 7) / 8; i < 16; i++)
        ones[i] = 0;
    // 交替模式：0xAA...
    for (int i = 0; i < (bw + 7) / 8; i++)
        pat[i] = 0xAA;
    for (int i = (bw + 7) / 8; i < 16; i++)
        pat[i] = 0;
    // 最高字节的掩码
    int rem = bw % 8;
    if (rem != 0) {
        ones[(bw - 1) / 8] &= (1u << rem) - 1;
        pat[(bw - 1) / 8] &= (1u << rem) - 1;
    }

    c.setWire(i0->id(), ones);
    c.setWire(i1->id(), pat);
    c.tick();
    auto out = c.getWire(o->id());
    for (int i = 0; i < (bw + 7) / 8; i++)
        EXPECT_EQ(out[i], ones[i] & pat[i]) << "位宽=" << bw << " 字节=" << i;
    for (int i = (bw + 7) / 8; i < 16; i++)
        EXPECT_EQ(out[i], 0) << "位宽=" << bw << " 高位字节=" << i << " 应为0";
}

TEST(BitWidthGate, BW_1) {
    gateBitWidthTest(1);
}
TEST(BitWidthGate, BW_7) {
    gateBitWidthTest(7);
}
TEST(BitWidthGate, BW_8) {
    gateBitWidthTest(8);
}
TEST(BitWidthGate, BW_9) {
    gateBitWidthTest(9);
}
TEST(BitWidthGate, BW_15) {
    gateBitWidthTest(15);
}
TEST(BitWidthGate, BW_16) {
    gateBitWidthTest(16);
}
TEST(BitWidthGate, BW_17) {
    gateBitWidthTest(17);
}
TEST(BitWidthGate, BW_31) {
    gateBitWidthTest(31);
}
TEST(BitWidthGate, BW_32) {
    gateBitWidthTest(32);
}
TEST(BitWidthGate, BW_33) {
    gateBitWidthTest(33);
}
TEST(BitWidthGate, BW_63) {
    gateBitWidthTest(63);
}
TEST(BitWidthGate, BW_64) {
    gateBitWidthTest(64);
}

// 测试各关键位宽的加法器，特别是 64 位进位检测
static void adderTest(int bw, uint64_t a, uint64_t b, bool cin, uint64_t expSum, bool expCout) {
    dsc::Circuit c;
    auto *na = c.createNet("a"), *nb = c.createNet("b"), *nc = c.createNet("cin");
    auto *ns = c.createNet("sum"), *no = c.createNet("cout");
    auto *ad = c.addComponent(std::make_unique<dsc::Adder>("ad", bw));
    c.connect(ad, "a", na);
    c.connect(ad, "b", nb);
    c.connect(ad, "cin", nc);
    c.connect(ad, "sum", ns);
    c.connect(ad, "cout", no);
    c.compile();
    c.init();

    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> va(16, 0), vb(16, 0);
    for (int i = 0; i < bytes; i++) {
        va[i] = (a >> (i * 8)) & 0xFF;
        vb[i] = (b >> (i * 8)) & 0xFF;
    }
    c.setWire(na->id(), va);
    c.setWire(nb->id(), vb);
    c.setWire(nc->id(), {static_cast<uint8_t>(cin ? 1 : 0), 0});
    c.tick();

    auto sumOut = c.getWire(ns->id());
    uint64_t gotSum = 0;
    for (int i = 0; i < bytes; i++)
        gotSum |= (uint64_t) sumOut[i] << (i * 8);
    EXPECT_EQ(gotSum, expSum) << "bw=" << bw << " a=" << a << " b=" << b << " cin=" << cin;

    auto coutOut = c.getWire(no->id());
    EXPECT_EQ(coutOut[0] != 0, expCout) << "bw=" << bw << " a=" << a << " b=" << b << " cin=" << cin;
}

TEST(BitWidthAdder, BW8_NoCarry) {
    adderTest(8, 0x12, 0x34, false, 0x46, false);
}
TEST(BitWidthAdder, BW8_WithCarry) {
    adderTest(8, 0xFF, 0x01, false, 0x00, true);
}
TEST(BitWidthAdder, BW8_Cin) {
    adderTest(8, 0xFE, 0x01, true, 0x00, true);
}
TEST(BitWidthAdder, BW16) {
    adderTest(16, 0xFFFF, 1, false, 0, true);
}
TEST(BitWidthAdder, BW32) {
    adderTest(32, 0xFFFFFFFFULL, 1, false, 0, true);
}
TEST(BitWidthAdder, BW63) {
    adderTest(63, (1ULL << 63) - 1, 1, false, 0, true);
}
TEST(BitWidthAdder, BW64_Overflow) {
    adderTest(64, 0xFFFFFFFFFFFFFFFFULL, 1, false, 0, true);
}
TEST(BitWidthAdder, BW64_Cin) {
    adderTest(64, 0xFFFFFFFFFFFFFFFEULL, 1, true, 0, true);
}
TEST(BitWidthAdder, BW64_Normal) {
    adderTest(64, 0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL, false, 0x123456789ABCDEF0ULL + 0x0FEDCBA987654321ULL,
              false);
}

// 测试 DFF 不同位宽
TEST(BitWidthSeq, DFF_BW1) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 1));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(clk->id(), {0, 0});
    c.setWire(d->id(), {1, 0});
    c.tick(); // clk=0，不触发
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 0);
    c.setWire(clk->id(), {1, 0});
    c.tick(); // 上升沿
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 1);
}

TEST(BitWidthSeq, DFF_BW32) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 32));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    std::vector<uint8_t> val(16, 0);
    val[0] = 0x78;
    val[1] = 0x56;
    val[2] = 0x34;
    val[3] = 0x12;
    c.setWire(clk->id(), {0, 0});
    c.setWire(d->id(), val);
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    auto out = c.getWire(q->id());
    EXPECT_EQ(out[0], 0x78);
    EXPECT_EQ(out[1], 0x56);
    EXPECT_EQ(out[2], 0x34);
    EXPECT_EQ(out[3], 0x12);
}

// 测试计数器不同位宽
TEST(BitWidthSeq, Counter_BW8_Wrap) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 8));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    for (int i = 0; i < 256; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    // 256 次后归零
    EXPECT_EQ(c.getWire(q->id())[0], 0);
}
