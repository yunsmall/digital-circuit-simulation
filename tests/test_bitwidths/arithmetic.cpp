// 算术元件全位宽测试：Adder, Subtractor, Multiplier, Divider, Mux, Comparator
#include <cstring>
#include "common.h"

// ---- 工具：构造指定位宽的最大值 ----
static uint64_t maxVal(int bw) {
    return (bw == 64) ? ~0ULL : (1ULL << bw) - 1;
}

// ---- 工具：uint64 → vector<uint8_t> ----
static std::vector<uint8_t> toBytes(int bw, uint64_t v) {
    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> b(16, 0);
    for (int i = 0; i < bytes; i++)
        b[i] = (v >> (i * 8)) & 0xFF;
    return b;
}

// ---- 工具：vector<uint8_t> → uint64_t ----
static uint64_t fromBytes(int bw, const std::vector<uint8_t> &b) {
    int bytes = (bw + 7) / 8;
    uint64_t v = 0;
    for (int i = 0; i < bytes && i < 8; i++)
        v |= (uint64_t) b[i] << (i * 8);
    return v;
}

// ============================================================
// Adder（扩展现有 adderTest，补全位宽）
// ============================================================
#define ALL_BW X(1) X(7) X(8) X(9) X(15) X(16) X(17) X(31) X(32) X(33) X(63) X(64)

TEST(BitWidthAdder, BW_Overflow) {
#define X(bw) adderTest(bw, maxVal(bw), 1, false, 0, true);
    ALL_BW
#undef X
}

// ============================================================
// Subtractor — 全位宽：0-1 → 借位+全1输出
// ============================================================
TEST(BitWidthSub, BW_Borrow) {
#define X(bw) subTest(bw, 0, 1, false, maxVal(bw), true);
    ALL_BW
#undef X
}

// ============================================================
// Multiplier — 全位宽：小乘法验证
// ============================================================
TEST(BitWidthMul, BW_SmallMul) {
    // 3×5=15: lo=15, hi=0（1-bit 时 1×1=1: lo=1, hi=0）
#define X(bw)                                                                                                          \
    if (bw == 1)                                                                                                       \
        mulTest(bw, 1, 1, 1, 0);                                                                                       \
    else                                                                                                               \
        mulTest(bw, 3, 5, 15, 0);
    ALL_BW
#undef X
}

// ============================================================
// Divider — 全位宽：除以 1 验证商=被除数
// ============================================================
static void divTest(int bw) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *q = c.createNet("quot"), *r = c.createNet("rem");
    auto *div = c.addComponent(std::make_unique<dsc::Divider>("div", bw));
    c.connect(div, "a", a);
    c.connect(div, "b", b);
    c.connect(div, "quot", q);
    c.connect(div, "rem", r);
    c.compile();
    c.init();
    uint64_t val = maxVal(bw);
    c.setWire(a->id(), toBytes(bw, val));
    c.setWire(b->id(), toBytes(bw, 1));
    c.tick();
    EXPECT_EQ(fromBytes(bw, c.getWire(q->id())), val) << "Div bw=" << bw;
    EXPECT_EQ(c.getWire(r->id())[0], 0) << "Div rem bw=" << bw;
}

TEST(BitWidthDiv, BW_DivByOne) {
#define X(bw) divTest(bw);
    ALL_BW
#undef X
}

// ============================================================
// Mux — 全位宽：sel=1 选 in1
// ============================================================
static void muxTest(int bw) {
    dsc::Circuit c;
    auto *ia = c.createNet("a"), *ib = c.createNet("b"), *s = c.createNet("sel"), *o = c.createNet("out");
    auto *m = c.addComponent(std::make_unique<dsc::Mux>("m", 1, bw));
    c.connect(m, "in0", ia);
    c.connect(m, "in1", ib);
    c.connect(m, "sel0", s);
    c.connect(m, "out", o);
    c.compile();
    c.init();
    uint64_t va = maxVal(bw), vb = 0;
    c.setWire(ia->id(), toBytes(bw, va));
    c.setWire(ib->id(), toBytes(bw, vb));
    c.setWire(s->id(), {0, 0});
    c.tick();
    EXPECT_EQ(fromBytes(bw, c.getWire(o->id())), va) << "Mux sel=0 bw=" << bw;
    c.setWire(s->id(), {1, 0});
    c.tick();
    EXPECT_EQ(fromBytes(bw, c.getWire(o->id())), vb) << "Mux sel=1 bw=" << bw;
}

TEST(BitWidthMux, BW_Full) {
#define X(bw) muxTest(bw);
    ALL_BW
#undef X
}

// ============================================================
// Comparator — 全位宽：EQ 验证
// ============================================================
static void cmpEqTest(int bw) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *cmp = c.addComponent(std::make_unique<dsc::Comparator>("cmp", bw, dsc::CmpOp::EQ));
    c.connect(cmp, "a", a);
    c.connect(cmp, "b", b);
    c.connect(cmp, "out", o);
    c.compile();
    c.init();
    uint64_t v = maxVal(bw);
    c.setWire(a->id(), toBytes(bw, v));
    c.setWire(b->id(), toBytes(bw, v));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1) << "CmpEQ eq bw=" << bw;
    c.setWire(b->id(), toBytes(bw, 0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0) << "CmpEQ ne bw=" << bw;
}

TEST(BitWidthCmp, BW_Full) {
#define X(bw) cmpEqTest(bw);
    ALL_BW
#undef X
}
