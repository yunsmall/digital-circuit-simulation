// 整型 → 浮点转换测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/FloatPoint.h>
#include <gtest/gtest.h>

// 辅助：写整型线网
static void writeWireInt(dsc::Circuit &c, int net_id, uint64_t val, int bit_w) {
    int bytes = (bit_w + 7) / 8;
    std::vector<uint8_t> buf(bytes, 0);
    for (int i = 0; i < bytes; i++)
        buf[i] = (val >> (i * 8)) & 0xFF;
    c.setWire(net_id, buf);
}

// 辅助：读浮点线网为 float
static float readWireF32(dsc::Circuit &c, int net_id) {
    auto buf = c.getWire(net_id);
    uint32_t u = 0;
    for (int i = 0; i < 4; i++)
        u |= (uint32_t) buf[i] << (i * 8);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// 辅助：读浮点线网为 double
static double readWireF64(dsc::Circuit &c, int net_id) {
    auto buf = c.getWire(net_id);
    uint64_t u = 0;
    for (int i = 0; i < 8; i++)
        u |= (uint64_t) buf[i] << (i * 8);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

// ============================================================
// IntToFloat — 无符号整型 → float32
// ============================================================
TEST(IntToFloatTest, U8ToF32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 8, 32, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 42, 8);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), 42.0f);
}

TEST(IntToFloatTest, U16ToF32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 16, 32, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 1000, 16);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), 1000.0f);
}

TEST(IntToFloatTest, U32ToF32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 32, 32, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 1234567, 32);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), 1234567.0f);
}

// ============================================================
// IntToFloat — 有符号整型 → float32
// ============================================================
TEST(IntToFloatTest, S8ToF32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 8, 32, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -5 → 0xFB（8位补码）
    writeWireInt(c, in->id(), 0xFB, 8);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), -5.0f);
}

TEST(IntToFloatTest, S16ToF32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 16, 32, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -32768 = 16位最小负值
    writeWireInt(c, in->id(), 0x8000, 16);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), -32768.0f);
}

TEST(IntToFloatTest, S8ToF32Positive) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 8, 32, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 127, 8);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), 127.0f);
}

// ============================================================
// IntToFloat — 无符号整型 → float64
// ============================================================
TEST(IntToFloatTest, U8ToF64) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 8, 64, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 99, 8);
    c.tick();
    EXPECT_DOUBLE_EQ(readWireF64(c, out->id()), 99.0);
}

// ============================================================
// IntToFloat — 有符号 → float64
// ============================================================
TEST(IntToFloatTest, S16ToF64) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 16, 64, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 0x8000, 16);
    c.tick();
    EXPECT_DOUBLE_EQ(readWireF64(c, out->id()), -32768.0);
}

// ============================================================
// IntToFloat — 边界：零值
// ============================================================
TEST(IntToFloatTest, Zero) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::IntToFloat>("i2f", 8, 32, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeWireInt(c, in->id(), 0, 8);
    c.tick();
    EXPECT_FLOAT_EQ(readWireF32(c, out->id()), 0.0f);
}
