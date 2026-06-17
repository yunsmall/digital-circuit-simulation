// MinMax 最小/最大值测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/Arithmetic.h>
#include <gtest/gtest.h>

static uint64_t readWireUint(dsc::Circuit &c, int net_id, int bit_w) {
    auto buf = c.getWire(net_id);
    uint64_t v = 0;
    for (int i = 0; i < (int) buf.size(); i++)
        v |= (uint64_t) buf[i] << (i * 8);
    if (bit_w < 64)
        v &= (1ULL << bit_w) - 1;
    return v;
}

static void writeWireUint(dsc::Circuit &c, int net_id, uint64_t val, int bit_w) {
    int bytes = (bit_w + 7) / 8;
    std::vector<uint8_t> buf(bytes, 0);
    for (int i = 0; i < bytes; i++)
        buf[i] = (val >> (i * 8)) & 0xFF;
    c.setWire(net_id, buf);
}

TEST(MinMaxTest, MinUnsigned) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::MinMax>("mm", 8, false, false));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    writeWireUint(c, a->id(), 10, 8);
    writeWireUint(c, b->id(), 200, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, o->id(), 8), 10);
}

TEST(MinMaxTest, MaxUnsigned) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::MinMax>("mm", 8, true, false));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    writeWireUint(c, a->id(), 10, 8);
    writeWireUint(c, b->id(), 200, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, o->id(), 8), 200);
}

TEST(MinMaxTest, MinSigned) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::MinMax>("mm", 8, false, true));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    // -5 (0xFB) < 3 → min = 0xFB
    writeWireUint(c, a->id(), 0xFB, 8);
    writeWireUint(c, b->id(), 3, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, o->id(), 8), 0xFB);
}

TEST(MinMaxTest, MaxSigned) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::MinMax>("mm", 8, true, true));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    // -5 (0xFB) vs 3 → signed compare: 0xFB(-5) < 3, max = 3
    writeWireUint(c, a->id(), 0xFB, 8);
    writeWireUint(c, b->id(), 3, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, o->id(), 8), 3);
}

TEST(MinMaxTest, EqualUnsigned) {
    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::MinMax>("mm", 16, false, false));
    c.connect(g, "a", a);
    c.connect(g, "b", b);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    writeWireUint(c, a->id(), 42, 16);
    writeWireUint(c, b->id(), 42, 16);
    c.tick();
    EXPECT_EQ(readWireUint(c, o->id(), 16), 42);
}
