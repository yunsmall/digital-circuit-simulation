// ALU：16 种操作码、位宽边界、溢出回绕、除零、有符号比较
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Alu.h>

using namespace dsc;

namespace {

void setU64(Circuit &c, Net *n, uint64_t v, int bytes) {
    std::vector<uint8_t> d(bytes);
    for (int i = 0; i < bytes; i++)
        d[i] = (uint8_t) (v >> (8 * i));
    c.setWire(n->id(), d);
}

uint64_t getU64(Circuit &c, Net *n) {
    auto v = c.getWire(n->id());
    uint64_t r = 0;
    for (size_t i = 0; i < v.size() && i < 8; i++)
        r |= (uint64_t) v[i] << (8 * i);
    return r;
}

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Alu *alu = nullptr;
    Net *a, *b, *op, *out, *zero;
    int bytes;

    explicit Fx(int bw) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<Alu>("alu0", bw);
        alu = p.get();
        c->addComponent(std::move(p));
        a = c->createNet("a");
        b = c->createNet("b");
        op = c->createNet("op");
        out = c->createNet("out");
        zero = c->createNet("zero");
        c->connect(alu, "a", a);
        c->connect(alu, "b", b);
        c->connect(alu, "op", op);
        c->connect(alu, "out", out);
        c->connect(alu, "zero", zero);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    uint64_t run(uint64_t av, uint64_t bv, uint32_t o) {
        setU64(*c, a, av, bytes);
        setU64(*c, b, bv, bytes);
        c->setWire(op->id(), {(uint8_t) (o & 0xF)});
        c->tick();
        return getU64(*c, out);
    }

    uint8_t zeroOf() const { return c->getWire(zero->id())[0] & 1; }
};

} // namespace

TEST(Alu, BasicOps8Bit) {
    Fx f(8);
    EXPECT_EQ(f.run(10, 3, 0), 13u);
    EXPECT_EQ(f.run(10, 3, 1), 7u);
    EXPECT_EQ(f.run(0xF0, 0x0F, 2), 0x00u);
    EXPECT_EQ(f.run(0xF0, 0x0F, 3), 0xFFu);
    EXPECT_EQ(f.run(0xF0, 0x0F, 4), 0xFFu);
    EXPECT_EQ(f.run(1, 3, 5), 8u);
    EXPECT_EQ(f.run(0x80, 1, 6), 0x40u);
    EXPECT_EQ(f.run(6, 2, 12), 12u);
    EXPECT_EQ(f.run(7, 2, 13), 3u);
    EXPECT_EQ(f.run(0x5A, 0, 14), 0x5Au);
    EXPECT_EQ(f.run(0x5A, 0, 15), 0xA5u);
}

TEST(Alu, NarrowWidthMasksResult) {
    Fx f(4);
    // 4 位宽下 0xF + 1 应回绕到 0，而不是留下第 5 位
    EXPECT_EQ(f.run(0xF, 1, 0), 0u);
    EXPECT_EQ(f.run(0xF, 2, 0), 1u);
    EXPECT_EQ(f.run(0x7, 0x7, 2), 7u);
}

TEST(Alu, SixtyFourBitWraps) {
    Fx f(64);
    EXPECT_EQ(f.run(0xFFFFFFFFFFFFFFFFull, 1, 0), 0u);
    EXPECT_EQ(f.run(0x8000000000000000ull, 1, 1), 0x7FFFFFFFFFFFFFFFull);
}

TEST(Alu, ShiftUsesLowSixBits) {
    Fx f(8);
    // 移位量 65 等价于 1（按 64 取模），再截到 8 位
    EXPECT_EQ(f.run(1, 65, 5), 2u);
    EXPECT_EQ(f.run(0x80, 65, 6), 0x40u);
}

TEST(Alu, ArithmeticShiftKeepsSign) {
    Fx f(8);
    EXPECT_EQ(f.run(0x80, 1, 7), 0xC0u);
    EXPECT_EQ(f.run(0x80, 7, 7), 0xFFu);
    EXPECT_EQ(f.run(0x7F, 1, 7), 0x3Fu);
}

TEST(Alu, SignedVsUnsignedLess) {
    Fx f(8);
    // 0x80 作为有符号数是 -128，作为无符号是 128
    EXPECT_EQ(f.run(0x80, 1, 8), 1u);
    EXPECT_EQ(f.run(0x80, 1, 9), 0u);
    EXPECT_EQ(f.run(1, 0x80, 8), 0u);
    EXPECT_EQ(f.run(1, 0x80, 9), 1u);
}

TEST(Alu, CompareOps) {
    Fx f(8);
    EXPECT_EQ(f.run(7, 7, 10), 1u);
    EXPECT_EQ(f.run(7, 8, 10), 0u);
    EXPECT_EQ(f.run(7, 8, 11), 1u);
    EXPECT_EQ(f.run(7, 7, 11), 0u);
}

TEST(Alu, DivideByZeroGivesAllOnes) {
    Fx f(8);
    EXPECT_EQ(f.run(42, 0, 13), 0xFFu);
    Fx f4(4);
    EXPECT_EQ(f4.run(9, 0, 13), 0xFu);
}

TEST(Alu, ZeroFlagTracksResult) {
    Fx f(8);
    f.run(1, 1, 1);
    EXPECT_EQ(f.zeroOf(), 1);
    f.run(2, 1, 1);
    EXPECT_EQ(f.zeroOf(), 0);
    // 结果被位宽截断成 0 时 zero 也要置位
    f.run(0x100, 0, 0);
    EXPECT_EQ(f.zeroOf(), 1);
}

TEST(Alu, OneBitWidth) {
    Fx f(1);
    EXPECT_EQ(f.run(1, 1, 0), 0u);
    EXPECT_EQ(f.run(1, 0, 0), 1u);
    EXPECT_EQ(f.run(1, 1, 2), 1u);
    EXPECT_EQ(f.run(1, 0, 2), 0u);
}

TEST(Alu, SignExtendIsPerWidth) {
    Fx f(16);
    EXPECT_EQ(f.run(0x8000, 1, 7), 0xC000u);
    EXPECT_EQ(f.run(0xFFFF, 1, 9), 0u); // 无符号比较：0xFFFF > 1
}
