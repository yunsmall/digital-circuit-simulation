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

// ---- 无符号整数转浮点（≥ 2^63 的半区）----
// 借道 int64_t 会变成负数，这是最容易漏的一档
TEST(FloatPointTest, IntToFloatUnsignedLarge) {
    dsc::Circuit c;
    auto *i = c.createNet("i");
    auto *o = c.createNet("o");
    auto *cv = c.addComponent(std::make_unique<dsc::IntToFloat>("cv", 64, 32, false));
    c.connect(cv, "in", i);
    c.connect(cv, "out", o);
    c.compile();
    c.init();

    auto out_f = [&] {
        auto v = c.getWire(o->id());
        uint32_t u = uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    };

    // 0x8000000000000000 = 2^63，转成 float 应约 9.22e18（正数，不能是负的）
    std::vector<uint8_t> hi(16, 0);
    hi[7] = 0x80;
    c.setWire(i->id(), hi);
    c.tick();
    EXPECT_GT(out_f(), 0.0f) << "2^63 转浮点必须是正数";
    EXPECT_NEAR(out_f() / 9.223372e18f, 1.0f, 0.01f) << "2^63 的量级要正确";
}

