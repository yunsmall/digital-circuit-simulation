// 浮点除法测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/FloatPoint.h>
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
    auto *g = c.addComponent(std::make_unique<dsc::FloatDiv>("fd", 32));
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
