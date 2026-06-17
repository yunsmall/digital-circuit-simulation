// Abs 绝对值测试
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

TEST(AbsTest, Positive8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Abs>("abs", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 5 → |5| = 5
    writeWireUint(c, in->id(), 5, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 5);
}

TEST(AbsTest, Negative8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Abs>("abs", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -5 = 0xFB → | -5 | = 5
    writeWireUint(c, in->id(), 0xFB, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 5);
}

TEST(AbsTest, Negative16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Abs>("abs", 16));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -100 = 0xFF9C → 100
    writeWireUint(c, in->id(), 0xFF9C, 16);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 16), 100);
}

TEST(AbsTest, MinNegative8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Abs>("abs", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -128 = 0x80 → 128（刚好不溢出）
    writeWireUint(c, in->id(), 0x80, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 128);
}

TEST(AbsTest, Zero8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Abs>("abs", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireUint(c, in->id(), 0, 8);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0);
}
