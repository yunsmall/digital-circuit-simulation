// BarrelShifter — 桶形移位器测试
#include <dcs/Circuit.h>
#include <dcs/components/BarrelShifter.h>
#include <gtest/gtest.h>

static void setWireVal(dsc::Circuit &c, int id, uint64_t val, int bw) {
    std::vector<uint8_t> v(16, 0);
    for (int i = 0; i < (bw + 7) / 8; i++)
        v[i] = (val >> (i * 8)) & 0xFF;
    c.setWire(id, v);
}
static void setWireBit(dsc::Circuit &c, int id, bool b) {
    c.setWire(id, {static_cast<uint8_t>(b ? 1 : 0), 0});
}
static uint64_t getWireVal(dsc::Circuit &c, int id, int bw) {
    auto v = c.getWire(id);
    uint64_t r = 0;
    for (int i = 0; i < (bw + 7) / 8 && i < 8; i++)
        r |= (uint64_t) v[i] << (i * 8);
    return r;
}

// 构建并编译一个 8 位桶形移位器
struct Barrel8Setup {
    dsc::Circuit c;
    dsc::BarrelShifter *bs;
    int ni, na, nd, nar, no;
    Barrel8Setup() {
        ni = c.createNet("in")->id();
        na = c.createNet("amt")->id();
        nd = c.createNet("dir")->id();
        nar = c.createNet("arith")->id();
        no = c.createNet("out")->id();
        bs = static_cast<dsc::BarrelShifter *>(c.addComponent(std::make_unique<dsc::BarrelShifter>("bs", 8)));
        c.connect(bs, "in", c.findNet("in"));
        c.connect(bs, "amt", c.findNet("amt"));
        c.connect(bs, "dir", c.findNet("dir"));
        c.connect(bs, "arith", c.findNet("arith"));
        c.connect(bs, "out", c.findNet("out"));
        c.compile();
        c.init();
    }
    void run(uint64_t in, uint64_t amt, bool dir, bool arith) {
        setWireVal(c, ni, in, 8);
        setWireVal(c, na, amt, 3); // 8位需要3位移位量
        setWireBit(c, nd, dir);
        setWireBit(c, nar, arith);
        c.tick();
    }
    uint64_t out() {
        return getWireVal(c, no, 8);
    }
};

TEST(BarrelShifter, LeftShift) {
    Barrel8Setup s;
    s.run(0x01, 3, false, false);
    EXPECT_EQ(s.out(), 0x08); // 0b00000001 << 3 = 0b00001000
}

TEST(BarrelShifter, LeftShiftWrapMask) {
    Barrel8Setup s;
    s.run(0x81, 2, false, false);
    EXPECT_EQ(s.out(), 0x04); // 0b10000001 << 2 = 0b00000100（高位截断）
}

TEST(BarrelShifter, RightLogical) {
    Barrel8Setup s;
    s.run(0x81, 2, true, false);
    EXPECT_EQ(s.out(), 0x20); // 0b10000001 >> 2 逻辑 = 0b00100000
}

TEST(BarrelShifter, RightArithmetic) {
    Barrel8Setup s;
    s.run(0x81, 2, true, true);
    EXPECT_EQ(s.out(), 0xE0); // 0b10000001 >> 2 算术 = 0b11100000（符号位1）
}

TEST(BarrelShifter, RightArithmeticPositive) {
    Barrel8Setup s;
    s.run(0x41, 2, true, true);
    EXPECT_EQ(s.out(), 0x10); // 0b01000001 >> 2 算术 = 0b00010000（符号位0）
}

TEST(BarrelShifter, ShiftZero) {
    Barrel8Setup s;
    s.run(0xAA, 0, false, false);
    EXPECT_EQ(s.out(), 0xAA); // 不移位
}

TEST(BarrelShifter, ShiftMaxLeft) {
    Barrel8Setup s;
    s.run(0x01, 7, false, false);
    EXPECT_EQ(s.out(), 0x80); // 移到最高位
}
