// Parity：偶/奇校验、位宽边界、高位截断
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Parity.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Parity *p = nullptr;
    Net *in, *out;
    int bytes;

    Fx(int bw, bool odd) : bytes((bw + 7) / 8) {
        auto q = std::make_unique<Parity>("par0", bw, odd);
        p = q.get();
        c->addComponent(std::move(q));
        in = c->createNet("in");
        out = c->createNet("out");
        c->connect(p, "in", in);
        c->connect(p, "out", out);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    uint8_t run(uint64_t v) {
        std::vector<uint8_t> d(bytes);
        for (int i = 0; i < bytes; i++)
            d[i] = (uint8_t) (v >> (8 * i));
        c->setWire(in->id(), d);
        c->tick();
        return c->getWire(out->id())[0] & 1;
    }
};

} // namespace

TEST(Parity, EvenCountsOneBits) {
    Fx f(8, false);
    EXPECT_EQ(f.run(0x00), 0u);
    EXPECT_EQ(f.run(0x01), 1u);
    EXPECT_EQ(f.run(0x03), 0u);
    EXPECT_EQ(f.run(0x07), 1u);
    EXPECT_EQ(f.run(0xFF), 0u);
}

TEST(Parity, OddIsInverse) {
    Fx f(8, true);
    EXPECT_EQ(f.run(0x00), 1u);
    EXPECT_EQ(f.run(0x01), 0u);
    EXPECT_EQ(f.run(0x03), 1u);
    EXPECT_EQ(f.run(0xFF), 1u);
}

TEST(Parity, OneBitWidth) {
    Fx f(1, false);
    EXPECT_EQ(f.run(0), 0u);
    EXPECT_EQ(f.run(1), 1u);
}

TEST(Parity, SixtyFourBits) {
    Fx f(64, false);
    EXPECT_EQ(f.run(0xFFFFFFFFFFFFFFFFull), 0u); // 64 个 1
    EXPECT_EQ(f.run(0xFFFFFFFFFFFFFFFEull), 1u); // 63 个 1
    EXPECT_EQ(f.run(0x8000000000000000ull), 1u);
}

TEST(Parity, FourBitWidth) {
    Fx f(4, false);
    EXPECT_EQ(f.run(0x0), 0u);
    EXPECT_EQ(f.run(0xF), 0u); // 4 个 1，偶校验为 0
    EXPECT_EQ(f.run(0x7), 1u); // 3 个 1
}
