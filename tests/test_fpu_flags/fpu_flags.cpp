// 浮点运算异常标志：除零、0/0、NaN、±Inf
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/FloatPoint.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    FloatBinOp *f = nullptr;
    Net *a, *b, *out, *flags;
    int prec, bytes;

    Fx(int precision, FloatBinOpKind op) : prec(precision), bytes(precision / 8) {
        auto p = std::make_unique<FloatBinOp>("f0", precision, op);
        f = p.get();
        c->addComponent(std::move(p));
        a = c->createNet("a");
        b = c->createNet("b");
        out = c->createNet("out");
        flags = c->createNet("flags");
        c->connect(f, "a", a);
        c->connect(f, "b", b);
        c->connect(f, "out", out);
        c->connect(f, "flags", flags);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    template<typename T>
    void set(Net *n, T v) {
        std::vector<uint8_t> d(bytes);
        std::memcpy(d.data(), &v, bytes);
        c->setWire(n->id(), d);
    }

    template<typename T>
    T get(Net *n) {
        auto d = c->getWire(n->id());
        T v{};
        std::memcpy(&v, d.data(), bytes);
        return v;
    }

    uint8_t run(float av, float bv) {
        set(a, av);
        set(b, bv);
        c->tick();
        return c->getWire(flags->id())[0];
    }

    uint8_t rund(double av, double bv) {
        set(a, av);
        set(b, bv);
        c->tick();
        return c->getWire(flags->id())[0];
    }
};

} // namespace

TEST(FpuFlags, NormalOperationSetsNothing) {
    Fx f(32, FloatBinOpKind::DIV);
    EXPECT_EQ(f.run(6.0f, 2.0f), 0u);
    EXPECT_FLOAT_EQ(f.get<float>(f.out), 3.0f);
}

TEST(FpuFlags, DivideByZeroIsPositiveInf) {
    Fx f(32, FloatBinOpKind::DIV);
    const uint8_t fl = f.run(1.0f, 0.0f);
    EXPECT_EQ(fl & 0x02u, 0x02u); // DZ
    EXPECT_EQ(fl & 0x40u, 0x40u); // +Inf
}

TEST(FpuFlags, DivideNegativeByZeroIsNegativeInf) {
    Fx f(32, FloatBinOpKind::DIV);
    const uint8_t fl = f.run(-1.0f, 0.0f);
    EXPECT_EQ(fl & 0x02u, 0x02u);
    EXPECT_EQ(fl & 0x80u, 0x80u); // -Inf
}

TEST(FpuFlags, ZeroOverZeroIsInvalidNan) {
    Fx f(32, FloatBinOpKind::DIV);
    const uint8_t fl = f.run(0.0f, 0.0f);
    EXPECT_EQ(fl & 0x01u, 0x01u); // NV
    EXPECT_EQ(fl & 0x20u, 0x20u); // NaN
    EXPECT_EQ(fl & 0x02u, 0u);    // 不是除零
}

TEST(FpuFlags, OverflowProducesInfinity) {
    Fx f(32, FloatBinOpKind::ADD);
    const uint8_t fl = f.run(3.0e38f, 3.0e38f);
    EXPECT_EQ(fl & 0x40u, 0x40u);
}

TEST(FpuFlags, NanInputPropagates) {
    Fx f(32, FloatBinOpKind::MUL);
    const uint8_t fl = f.run(0.0f / 0.0f, 1.0f);
    EXPECT_EQ(fl & 0x20u, 0x20u);
}

TEST(FpuFlags, DoublePrecisionDivideByZero) {
    Fx f(64, FloatBinOpKind::DIV);
    const uint8_t fl = f.rund(2.0, 0.0);
    EXPECT_EQ(fl & 0x02u, 0x02u);
    EXPECT_EQ(fl & 0x40u, 0x40u);
}

TEST(FpuFlags, DoublePrecisionNormal) {
    Fx f(64, FloatBinOpKind::MUL);
    EXPECT_EQ(f.rund(2.5, 4.0), 0u);
    EXPECT_DOUBLE_EQ(f.get<double>(f.out), 10.0);
}
