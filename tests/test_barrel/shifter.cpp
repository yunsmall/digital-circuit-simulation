// 移位器测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/Shifter.h>
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

// ============================================================
// LSL — 逻辑左移
// ============================================================
TEST(ShifterTest, LSL8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 8, dsc::ShiftMode::LSL));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0b00101010 << 2 = 0b10101000
    writeWireUint(c, in->id(), 0x2A, 8);
    writeWireUint(c, sh->id(), 2, 3);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0xA8);
}

TEST(ShifterTest, LSLoverflow) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 8, dsc::ShiftMode::LSL));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0xFF << 4 = 0xF0（高位移出丢弃）
    writeWireUint(c, in->id(), 0xFF, 8);
    writeWireUint(c, sh->id(), 4, 3);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0xF0);
}

// ============================================================
// LSR — 逻辑右移
// ============================================================
TEST(ShifterTest, LSR8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 8, dsc::ShiftMode::LSR));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0b10101000 >> 2 = 0b00101010
    writeWireUint(c, in->id(), 0xA8, 8);
    writeWireUint(c, sh->id(), 2, 3);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0x2A);
}

// ============================================================
// ASR — 算术右移
// ============================================================
TEST(ShifterTest, ASR8positive) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 8, dsc::ShiftMode::ASR));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 正数：0b00101000 >> 2 = 0b00001010
    writeWireUint(c, in->id(), 0x28, 8);
    writeWireUint(c, sh->id(), 2, 3);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0x0A);
}

TEST(ShifterTest, ASR8negative) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 8, dsc::ShiftMode::ASR));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 负数：0b10101000 >> 2 = 0b11101010（高位填符号位 1）
    writeWireUint(c, in->id(), 0xA8, 8);
    writeWireUint(c, sh->id(), 2, 3);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0xEA);
}

TEST(ShifterTest, ASR16negative) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 16, dsc::ShiftMode::ASR));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0xFF00 >> 4 = 0xFFF0（16位算术右移）
    writeWireUint(c, in->id(), 0xFF00, 16);
    writeWireUint(c, sh->id(), 4, 4);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 16), 0xFFF0);
}

TEST(ShifterTest, ShiftZero) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *sh = c.createNet("sh"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Shifter>("sft", 8, dsc::ShiftMode::LSL));
    c.connect(g, "in", in);
    c.connect(g, "shift", sh);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 不移
    writeWireUint(c, in->id(), 0x55, 8);
    writeWireUint(c, sh->id(), 0, 3);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0x55);
}
