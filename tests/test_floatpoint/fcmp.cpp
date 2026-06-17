// 浮点比较测试
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

static std::vector<uint8_t> f32bytes(double v) {
    uint64_t u = f32(v);
    std::vector<uint8_t> b(4);
    for (int i = 0; i < 4; i++)
        b[i] = (u >> (i * 8)) & 0xFF;
    return b;
}

TEST(FloatCmpTest, EQ) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatCmp>("fc", 32, dsc::FloatCmpOp::EQ));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), f32bytes(2.5));
    c.setWire(b->id(), f32bytes(2.5));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(FloatCmpTest, NE) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatCmp>("fc", 32, dsc::FloatCmpOp::NE));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), f32bytes(2.5));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(b->id(), f32bytes(2.5));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(FloatCmpTest, LT) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatCmp>("fc", 32, dsc::FloatCmpOp::LT));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), f32bytes(1.0));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), f32bytes(5.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(FloatCmpTest, GT) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatCmp>("fc", 32, dsc::FloatCmpOp::GT));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), f32bytes(5.0));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), f32bytes(1.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(FloatCmpTest, LE) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatCmp>("fc", 32, dsc::FloatCmpOp::LE));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), f32bytes(1.0));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), f32bytes(3.0));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), f32bytes(5.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

TEST(FloatCmpTest, GE) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatCmp>("fc", 32, dsc::FloatCmpOp::GE));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(a->id(), f32bytes(5.0));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
    c.setWire(a->id(), f32bytes(3.0));
    c.setWire(b->id(), f32bytes(3.0));
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 1);
}
