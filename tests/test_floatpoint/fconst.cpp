// 浮点常量测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/FloatPoint.h>
#include <gtest/gtest.h>

TEST(FloatConstTest, Const32) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatConst>("fc", 32, 3.14159));
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.tick();
    float v;
    uint32_t u = 0;
    auto bytes = c.getWire(o->id());
    for (int i = 0; i < 4 && i < (int) bytes.size(); i++)
        u |= (uint32_t) bytes[i] << (i * 8);
    std::memcpy(&v, &u, 4);
    EXPECT_NEAR(v, 3.14159, 0.001);
}

TEST(FloatConstTest, Const64) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatConst>("fc", 64, 2.718281828));
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.tick();
    double v;
    uint64_t u = 0;
    auto bytes = c.getWire(o->id());
    for (int i = 0; i < 8 && i < (int) bytes.size(); i++)
        u |= (uint64_t) bytes[i] << (i * 8);
    std::memcpy(&v, &u, 8);
    EXPECT_NEAR(v, 2.718281828, 0.0000001);
}
