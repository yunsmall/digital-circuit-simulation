// 128 位辅助函数：MSVC 没有 __int128，CPU 的 MULH 系列、clmul、非对齐访存
// 都靠 dcs/util/Int128.h 里的手写实现。这里用已知值和内建类型双重验证。
#include <dcs/util/Int128.h>
#include <gtest/gtest.h>
#include <cstdint>

using namespace dsc;

TEST(Int128, Mulhu64KnownValues) {
    EXPECT_EQ(mulhu64(0, 0), 0u);
    EXPECT_EQ(mulhu64(1, 1), 0u);
    EXPECT_EQ(mulhu64(UINT64_MAX, 1), 0u);
    // (2^64-1)^2 = 2^128 - 2^65 + 1，高 64 位是 2^64-2
    EXPECT_EQ(mulhu64(UINT64_MAX, UINT64_MAX), UINT64_MAX - 1);
    EXPECT_EQ(mulhu64(UINT64_MAX, 2), 1u);
}

TEST(Int128, Mulh64KnownValues) {
    EXPECT_EQ(mulh64(-1, -1), 0u);                       // 1 的高半
    EXPECT_EQ(mulh64(1, -1), UINT64_MAX);                // -1 的高半全 1
    EXPECT_EQ(mulh64(INT64_MIN, 1), UINT64_MAX);         // -2^63 的高半全 1
    EXPECT_EQ(mulh64(INT64_MIN, -1), 0u);                // 2^63 的高半是 0
    EXPECT_EQ(mulh64(INT64_MIN, 2), UINT64_MAX);         // -2^64 的高半是 -1
}

TEST(Int128, Mulhsu64KnownValues) {
    EXPECT_EQ(mulhsu64(1, 2), 0u);
    EXPECT_EQ(mulhsu64(-1, 2), UINT64_MAX);              // -2 的高半全 1
    EXPECT_EQ(mulhsu64(INT64_MIN, 2), UINT64_MAX);       // -2^64 的高半是 -1
    EXPECT_EQ(mulhsu64(INT64_MIN, 1), UINT64_MAX);       // -2^63 的高半全 1
}

TEST(Int128, Mul64x64ReturnsLowHalf) {
    uint64_t hi = 0;
    EXPECT_EQ(mul64x64(UINT64_MAX, UINT64_MAX, hi), 1u); // (2^128-2^65+1) 的低位
    EXPECT_EQ(hi, UINT64_MAX - 1);
}

TEST(Int128, U128OrAndShift) {
    U128 a;
    a.set(UINT64_MAX);
    EXPECT_EQ(a.lo, UINT64_MAX);
    EXPECT_EQ(a.hi, 0u);
    a.or_shifted(1, 64);                                 // 置 bit64
    EXPECT_EQ(a.hi, 1u);
    EXPECT_EQ(a.shift_right_lo(64), 1u);
    EXPECT_EQ(a.shift_right_lo(0), UINT64_MAX);
    // 移位量 < 64 时高低字要拼接
    EXPECT_EQ(a.shift_right_lo(1), ((UINT64_MAX >> 1) | (1ULL << 63)));
}

// GCC/Clang 有 __int128，直接与内建类型对照；MSVC 下这段不编译，
// 由上面的已知值测试覆盖
#if defined(__SIZEOF_INT128__)
TEST(Int128, MatchesBuiltinForClmul) {
    const uint64_t pairs[][2] = {
            {0, 0},
            {1, 1},
            {UINT64_MAX, UINT64_MAX},
            {0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL},
            {0x8000000000000001ULL, 0x8000000000000001ULL},
    };
    for (auto &p: pairs) {
        const uint64_t v1 = p[0], v2 = p[1];
        __uint128_t ref = 0;
        U128 got;
        for (int b = 0; b < 64; ++b) {
            if ((v2 >> b) & 1) {
                ref ^= (__uint128_t) v1 << b;
                got.xor_shifted(v1, b);
            }
        }
        EXPECT_EQ(got.lo, (uint64_t) ref);
        EXPECT_EQ(got.hi, (uint64_t) (ref >> 64));
        for (int s = 0; s < 128; s += 17)
            EXPECT_EQ(got.shift_right_lo(s), (uint64_t) (ref >> s)) << "s=" << s;
    }
}

TEST(Int128, MatchesBuiltinForMul) {
    const uint64_t vals[] = {0, 1, 2, UINT64_MAX, (uint64_t) INT64_MIN,
                             0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL, 0x123456789ABCDEF0ULL};
    for (uint64_t a: vals) {
        for (uint64_t b: vals) {
            const __uint128_t up = (__uint128_t) a * b;
            EXPECT_EQ(mulhu64(a, b), (uint64_t) (up >> 64));
            const __int128 sp = (__int128) (int64_t) a * (__int128) (int64_t) b;
            EXPECT_EQ(mulh64((int64_t) a, (int64_t) b), (uint64_t) (sp >> 64));
            const __int128 sup = (__int128) (int64_t) a * (__uint128_t) b;
            EXPECT_EQ(mulhsu64((int64_t) a, b), (uint64_t) (sup >> 64));
        }
    }
}
#endif
