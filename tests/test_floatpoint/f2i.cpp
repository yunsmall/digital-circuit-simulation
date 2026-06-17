// 浮点 → 整型转换测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/FloatPoint.h>
#include <gtest/gtest.h>

// 辅助：写浮点线网（32位）
static void writeFloat32(dsc::Circuit &c, int net_id, float val) {
    uint32_t u;
    std::memcpy(&u, &val, 4);
    std::vector<uint8_t> buf(4);
    for (int i = 0; i < 4; i++)
        buf[i] = (u >> (i * 8)) & 0xFF;
    c.setWire(net_id, buf);
}

// 辅助：写浮点线网（64位）
static void writeFloat64(dsc::Circuit &c, int net_id, double val) {
    uint64_t u;
    std::memcpy(&u, &val, 8);
    std::vector<uint8_t> buf(8);
    for (int i = 0; i < 8; i++)
        buf[i] = (u >> (i * 8)) & 0xFF;
    c.setWire(net_id, buf);
}

// 辅助：读整型线网
static uint64_t readWireInt(dsc::Circuit &c, int net_id, int bit_w) {
    auto buf = c.getWire(net_id);
    uint64_t val = 0;
    for (int i = 0; i < (bit_w + 7) / 8 && i < (int) buf.size(); i++)
        val |= (uint64_t) buf[i] << (i * 8);
    if (bit_w < 64)
        val &= (1ULL << bit_w) - 1;
    return val;
}

// ============================================================
// FloatToInt — float32 → 无符号整型
// ============================================================
TEST(FloatToIntTest, F32ToU8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 8, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 42.0f → 42
    writeFloat32(c, in->id(), 42.0f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 8), 42);
}

TEST(FloatToIntTest, F32ToU16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 16, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeFloat32(c, in->id(), 255.0f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 16), 255);
}

TEST(FloatToIntTest, F32ToU32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 32, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeFloat32(c, in->id(), 100000.0f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 32), 100000);
}

// ============================================================
// FloatToInt — float32 → 有符号整型
// ============================================================
TEST(FloatToIntTest, F32ToS8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 8, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -5.0 → 0xFB (补码 -5 的 8 位表示)
    writeFloat32(c, in->id(), -5.0f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 8), 0xFB);
}

TEST(FloatToIntTest, F32ToS16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 16, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // -100 → 16位有符号
    writeFloat32(c, in->id(), -100.0f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 16), (uint64_t)(uint16_t)(int16_t) - 100); // 0xFF9C
}

TEST(FloatToIntTest, F32ToS8Positive) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 8, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeFloat32(c, in->id(), 100.0f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 8), 100);
}

// ============================================================
// FloatToInt — 截断行为
// ============================================================
TEST(FloatToIntTest, TruncateFraction32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 32, 8, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 3.9f → 截断为 3
    writeFloat32(c, in->id(), 3.9f);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 8), 3);
}

// ============================================================
// FloatToInt — float64
// ============================================================
TEST(FloatToIntTest, F64ToU8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 64, 8, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeFloat64(c, in->id(), 127.0);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 8), 127);
}

TEST(FloatToIntTest, F64ToS16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::FloatToInt>("f2i", 64, 16, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    writeFloat64(c, in->id(), -32768.0);
    c.tick();
    EXPECT_EQ(readWireInt(c, out->id(), 16), 0x8000);
}
