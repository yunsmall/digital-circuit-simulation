// 浮点加法测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/FloatPoint.h>
#include <gtest/gtest.h>

// 辅助：float/double 的 IEEE 754 位表示
static uint64_t f32(double v) {
    float f = (float) v;
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

TEST(FloatAddTest, BasicAdd32) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatBinOp>("fa", 32, dsc::FloatBinOpKind::ADD));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    // 2.5 + 3.5 = 6.0
    std::vector<uint8_t> va(4), vb(4);
    uint64_t ua = f32(2.5), ub = f32(3.5);
    for (int i = 0; i < 4; i++) {
        va[i] = (ua >> (i * 8)) & 0xFF;
        vb[i] = (ub >> (i * 8)) & 0xFF;
    }
    c.setWire(a->id(), va);
    c.setWire(b->id(), vb);
    c.tick();
    uint64_t uo = f32(6.0);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(c.getWire(o->id())[i], (uo >> (i * 8)) & 0xFF);
}

TEST(FloatAddTest, Negate32) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatBinOp>("fa", 32, dsc::FloatBinOpKind::ADD));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    // 5.0 + (-5.0) = 0.0
    std::vector<uint8_t> va(4), vb(4);
    uint64_t ua = f32(5.0), ub = f32(-5.0);
    for (int i = 0; i < 4; i++) {
        va[i] = (ua >> (i * 8)) & 0xFF;
        vb[i] = (ub >> (i * 8)) & 0xFF;
    }
    c.setWire(a->id(), va);
    c.setWire(b->id(), vb);
    c.tick();
    uint64_t uo = f32(0.0);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(c.getWire(o->id())[i], (uo >> (i * 8)) & 0xFF);
}

TEST(FloatAddTest, BasicAdd64) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatBinOp>("fa", 64, dsc::FloatBinOpKind::ADD));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    double da = 1.5, db = 2.5;
    std::vector<uint8_t> va(8), vb(8);
    uint64_t ua, ub;
    std::memcpy(&ua, &da, 8);
    std::memcpy(&ub, &db, 8);
    for (int i = 0; i < 8; i++) {
        va[i] = (ua >> (i * 8)) & 0xFF;
        vb[i] = (ub >> (i * 8)) & 0xFF;
    }
    c.setWire(a->id(), va);
    c.setWire(b->id(), vb);
    c.tick();
    double dout = 4.0;
    uint64_t uo;
    std::memcpy(&uo, &dout, 8);
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(c.getWire(o->id())[i], (uo >> (i * 8)) & 0xFF);
}
