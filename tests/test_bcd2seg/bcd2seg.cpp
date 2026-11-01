// Bcd2Seg：0~9、A~F、非 BCD、共阳极性、小数点
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/io/Bcd2Seg.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Bcd2Seg *d = nullptr;
    Net *in, *dp, *seg;

    Fx(bool hex_mode, bool active_low) {
        auto p = std::make_unique<Bcd2Seg>("seg0", hex_mode, active_low);
        d = p.get();
        c->addComponent(std::move(p));
        in = c->createNet("in");
        dp = c->createNet("dp");
        seg = c->createNet("seg");
        c->connect(d, "in", in);
        c->connect(d, "dp", dp);
        c->connect(d, "seg", seg);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    uint8_t run(uint32_t v, bool point = false) {
        c->setWire(in->id(), (uint8_t) (v & 0xF));
        c->setWire(dp->id(), (uint8_t) (point ? 1 : 0));
        c->tick();
        return c->getWire(seg->id())[0];
    }
};

} // namespace

TEST(Bcd2Seg, DecimalDigits) {
    const uint8_t want[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    Fx f(false, false);
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(f.run((uint32_t) i), want[i]) << "digit " << i;
}

TEST(Bcd2Seg, HexDigitsOnlyInHexMode) {
    const uint8_t want[6] = {0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71};
    Fx hex(true, false), bcd(false, false);
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(hex.run((uint32_t) (10 + i)), want[i]) << "hex " << i;
        EXPECT_EQ(bcd.run((uint32_t) (10 + i)), 0) << "bcd " << i;
    }
}

TEST(Bcd2Seg, ActiveLowInvertsAllSegments) {
    Fx f(false, true);
    EXPECT_EQ(f.run(0), (uint8_t) ~0x3F);
    EXPECT_EQ(f.run(8), (uint8_t) ~0x7F);
}

TEST(Bcd2Seg, DecimalPointBit) {
    Fx f(false, false);
    EXPECT_EQ(f.run(8, true), 0xFF);
    EXPECT_EQ(f.run(8, false), 0x7F);
}

TEST(Bcd2Seg, ActiveLowDecimalPoint) {
    Fx f(false, true);
    // 共阳时小数点亮 = 该位低
    EXPECT_EQ(f.run(8, true), (uint8_t) (~0x7F & 0x7F));
    EXPECT_EQ(f.run(8, false), (uint8_t) ~0x7F);
}

TEST(Bcd2Seg, IgnoresBitsAboveNibble) {
    Fx f(false, false);
    // 输入引脚只有 4 位，高位的值不能影响译码
    EXPECT_EQ(f.run(0x0A), 0);
    EXPECT_EQ(f.run(0x1A), 0);
}
