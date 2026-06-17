// 桶形移位器测试（带方向/算术控制输入引脚）
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/BarrelShifter.h>
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
// 左移（dir=0）
// ============================================================
TEST(BarrelShifterTest, LeftShift8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *amt = c.createNet("amt"), *dir = c.createNet("dir"),
         *arith = c.createNet("arith"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 8));
    c.connect(g, "in", in);
    c.connect(g, "amt", amt);
    c.connect(g, "dir", dir);
    c.connect(g, "arith", arith);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0x2A << 2 = 0xA8
    writeWireUint(c, in->id(), 0x2A, 8);
    writeWireUint(c, amt->id(), 2, 3);
    writeWireUint(c, dir->id(), 0, 1);
    writeWireUint(c, arith->id(), 0, 1);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0xA8);
}

// ============================================================
// 逻辑右移（dir=1, arith=0）
// ============================================================
TEST(BarrelShifterTest, LogicalRightShift8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *amt = c.createNet("amt"), *dir = c.createNet("dir"),
         *arith = c.createNet("arith"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 8));
    c.connect(g, "in", in);
    c.connect(g, "amt", amt);
    c.connect(g, "dir", dir);
    c.connect(g, "arith", arith);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0xA8 >> 2 = 0x2A（高位补0）
    writeWireUint(c, in->id(), 0xA8, 8);
    writeWireUint(c, amt->id(), 2, 3);
    writeWireUint(c, dir->id(), 1, 1);
    writeWireUint(c, arith->id(), 0, 1);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0x2A);
}

// ============================================================
// 算术右移（dir=1, arith=1）—— 正数
// ============================================================
TEST(BarrelShifterTest, ArithRightShiftPositive) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *amt = c.createNet("amt"), *dir = c.createNet("dir"),
         *arith = c.createNet("arith"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 8));
    c.connect(g, "in", in);
    c.connect(g, "amt", amt);
    c.connect(g, "dir", dir);
    c.connect(g, "arith", arith);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 正数 0x28 >> 2 = 0x0A（高位补符号位0）
    writeWireUint(c, in->id(), 0x28, 8);
    writeWireUint(c, amt->id(), 2, 3);
    writeWireUint(c, dir->id(), 1, 1);
    writeWireUint(c, arith->id(), 1, 1);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0x0A);
}

// ============================================================
// 算术右移（dir=1, arith=1）—— 负数
// ============================================================
TEST(BarrelShifterTest, ArithRightShiftNegative) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *amt = c.createNet("amt"), *dir = c.createNet("dir"),
         *arith = c.createNet("arith"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 8));
    c.connect(g, "in", in);
    c.connect(g, "amt", amt);
    c.connect(g, "dir", dir);
    c.connect(g, "arith", arith);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 负数 0xA8 >> 2 = 0xEA（高位补符号位1）
    writeWireUint(c, in->id(), 0xA8, 8);
    writeWireUint(c, amt->id(), 2, 3);
    writeWireUint(c, dir->id(), 1, 1);
    writeWireUint(c, arith->id(), 1, 1);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0xEA);
}

// ============================================================
// 16 位算术右移
// ============================================================
TEST(BarrelShifterTest, ArithRightShift16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *amt = c.createNet("amt"), *dir = c.createNet("dir"),
         *arith = c.createNet("arith"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 16));
    c.connect(g, "in", in);
    c.connect(g, "amt", amt);
    c.connect(g, "dir", dir);
    c.connect(g, "arith", arith);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 0xFF00 >> 4 = 0xFFF0（16位算术右移）
    writeWireUint(c, in->id(), 0xFF00, 16);
    writeWireUint(c, amt->id(), 4, 4);
    writeWireUint(c, dir->id(), 1, 1);
    writeWireUint(c, arith->id(), 1, 1);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 16), 0xFFF0);
}

// ============================================================
// 移位量为 0
// ============================================================
TEST(BarrelShifterTest, ShiftZero) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *amt = c.createNet("amt"), *dir = c.createNet("dir"),
         *arith = c.createNet("arith"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 8));
    c.connect(g, "in", in);
    c.connect(g, "amt", amt);
    c.connect(g, "dir", dir);
    c.connect(g, "arith", arith);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    // 不移
    writeWireUint(c, in->id(), 0x55, 8);
    writeWireUint(c, amt->id(), 0, 3);
    writeWireUint(c, dir->id(), 0, 1);
    writeWireUint(c, arith->id(), 0, 1);
    c.tick();
    EXPECT_EQ(readWireUint(c, out->id(), 8), 0x55);
}
