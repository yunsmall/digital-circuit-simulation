// 浮点除法测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/combinational/FloatPoint.h>
#include <gtest/gtest.h>

static uint64_t f32(double v) {
    float f = (float) v;
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

TEST(FloatDivTest, BasicDiv32) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatBinOp>("fd", 32, dsc::FloatBinOpKind::DIV));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    // 6.0 / 2.0 = 3.0
    std::vector<uint8_t> va(4), vb(4);
    uint64_t ua = f32(6.0), ub = f32(2.0);
    for (int i = 0; i < 4; i++) {
        va[i] = (ua >> (i * 8)) & 0xFF;
        vb[i] = (ub >> (i * 8)) & 0xFF;
    }
    c.setWire(a->id(), va);
    c.setWire(b->id(), vb);
    c.tick();
    uint64_t uo = f32(3.0);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(c.getWire(o->id())[i], (uo >> (i * 8)) & 0xFF);
}

namespace {
// 跑一次 32 位除法，返回异常标志字节
uint8_t divFlags(uint32_t a_bits, uint32_t b_bits) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out"), *fl = c.createNet("flags");
    auto *g = c.addComponent(std::make_unique<dsc::FloatBinOp>("fd", 32, dsc::FloatBinOpKind::DIV));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.connect(g, "flags", fl);
    c.compile();
    c.init();
    std::vector<uint8_t> va(4), vb(4);
    for (int i = 0; i < 4; i++) {
        va[i] = (a_bits >> (i * 8)) & 0xFF;
        vb[i] = (b_bits >> (i * 8)) & 0xFF;
    }
    c.setWire(a->id(), va);
    c.setWire(b->id(), vb);
    c.tick();
    return c.getWire(fl->id())[0];
}
} // namespace

// IEEE 754：DZ 只针对有限非零被除数 / 0；Inf/0、NaN/0 都不应置 DZ
TEST(FloatDivTest, InfinityDivZeroDoesNotSetDz) {
    const uint8_t flags = divFlags(0x7F800000u /*+Inf*/, 0);
    EXPECT_EQ(flags & 0x02u, 0u) << "Inf/0 不触发除零异常";
    EXPECT_NE(flags & 0x40u, 0u) << "结果仍是 +Inf";
}

TEST(FloatDivTest, NaNInputDivZeroDoesNotSetDz) {
    const uint8_t flags = divFlags(0x7FC00000u /*NaN*/, 0);
    EXPECT_EQ(flags & 0x02u, 0u) << "NaN/0 不触发除零异常";
    EXPECT_NE(flags & 0x20u, 0u) << "结果仍是 NaN";
}

TEST(FloatDivTest, FiniteDivZeroSetsDz) {
    const float one = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &one, 4);
    const uint8_t flags = divFlags(bits, 0);
    EXPECT_NE(flags & 0x02u, 0u) << "有限非零 / 0 必须触发 DZ";
    EXPECT_NE(flags & 0x40u, 0u) << "结果应是 +Inf";
}

TEST(FloatDivTest, ZeroDivZeroIsInvalidNotDz) {
    const uint8_t flags = divFlags(0, 0);
    EXPECT_NE(flags & 0x01u, 0u) << "0/0 是 invalid";
    EXPECT_EQ(flags & 0x02u, 0u) << "0/0 不算除零异常";
}
