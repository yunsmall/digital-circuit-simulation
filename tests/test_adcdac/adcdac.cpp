// ADC/DAC：满量程映射、量化取整、越界饱和、vref=0
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/AdcDac.h>

using namespace dsc;

namespace {

struct DacFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Dac *d = nullptr;
    Net *value, *analog;
    int bytes;

    DacFx(int bw, double vref) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<Dac>("dac0", bw, vref);
        d = p.get();
        c->addComponent(std::move(p));
        value = c->createNet("value");
        analog = c->createNet("analog");
        c->connect(d, "value", value);
        c->connect(d, "analog", analog);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    float run(uint64_t v) {
        std::vector<uint8_t> data(bytes);
        for (int i = 0; i < bytes; i++)
            data[i] = (uint8_t) (v >> (8 * i));
        c->setWire(value->id(), data);
        c->tick();
        auto o = c->getWire(analog->id());
        float f = 0;
        std::memcpy(&f, o.data(), 4);
        return f;
    }
};

struct AdcFx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Adc *a = nullptr;
    Net *analog, *value;
    int bytes;

    AdcFx(int bw, double vref) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<Adc>("adc0", bw, vref);
        a = p.get();
        c->addComponent(std::move(p));
        analog = c->createNet("analog");
        value = c->createNet("value");
        c->connect(a, "analog", analog);
        c->connect(a, "value", value);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    uint64_t run(float f) {
        std::vector<uint8_t> d(4);
        std::memcpy(d.data(), &f, 4);
        c->setWire(analog->id(), d);
        c->tick();
        auto o = c->getWire(value->id());
        uint64_t r = 0;
        for (size_t i = 0; i < o.size() && i < 8; i++)
            r |= (uint64_t) o[i] << (8 * i);
        return r;
    }
};

} // namespace

TEST(Dac, EndpointsAndMiddle) {
    DacFx f(8, 5.0);
    EXPECT_FLOAT_EQ(f.run(0), 0.0f);
    EXPECT_FLOAT_EQ(f.run(255), 5.0f);
    EXPECT_NEAR(f.run(128), 5.0f * 128.0f / 255.0f, 1e-5f);
}

TEST(Dac, WideBitWidth) {
    DacFx f(16, 3.3);
    EXPECT_FLOAT_EQ(f.run(0), 0.0f);
    EXPECT_FLOAT_EQ(f.run(65535), 3.3f);
}

TEST(Adc, Endpoints) {
    AdcFx f(8, 5.0);
    EXPECT_EQ(f.run(0.0f), 0u);
    EXPECT_EQ(f.run(5.0f), 255u);
}

TEST(Adc, QuantizesAndRounds) {
    AdcFx f(8, 5.0);
    // 2.5V 正好半量程
    EXPECT_EQ(f.run(2.5f), 128u);
    EXPECT_EQ(f.run(0.01f), 1u); // 0.51 取整
}

TEST(Adc, SaturatesOutOfRange) {
    AdcFx f(8, 5.0);
    EXPECT_EQ(f.run(-1.0f), 0u);
    EXPECT_EQ(f.run(9.0f), 255u);
}

TEST(Adc, ZeroVrefGivesZero) {
    AdcFx f(8, 0.0);
    EXPECT_EQ(f.run(3.0f), 0u);
}

TEST(DacAdc, RoundTripWithinOneLsb) {
    DacFx d(8, 5.0);
    for (uint64_t v: {0ull, 1ull, 64ull, 128ull, 200ull, 255ull}) {
        AdcFx a(8, 5.0);
        const float volts = d.run(v);
        EXPECT_LE(std::llabs((long long) a.run(volts) - (long long) v), 1u) << "value " << v;
    }
}
