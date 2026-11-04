// CheckedInt 的边界测试：每个函数都要覆盖两侧极限、零、异号组合，
// 以及溢出时不得写 out
#include <cstdint>
#include <gtest/gtest.h>

#include <dcs/util/CheckedInt.h>

using namespace dsc::checked;

namespace {
constexpr int64_t kMax = INT64_MAX;
constexpr int64_t kMin = INT64_MIN;
} // namespace

TEST(CheckedAdd, NormalCases) {
    int64_t r = 0;
    EXPECT_FALSE(addOverflow(0, 0, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(addOverflow(1, -1, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(addOverflow(-5, 3, r));
    EXPECT_EQ(r, -2);
    EXPECT_FALSE(addOverflow(3, -5, r));
    EXPECT_EQ(r, -2);
}

TEST(CheckedAdd, Boundaries) {
    int64_t r = 0;
    EXPECT_FALSE(addOverflow(kMax, 0, r));
    EXPECT_EQ(r, kMax);
    EXPECT_FALSE(addOverflow(0, kMin, r));
    EXPECT_EQ(r, kMin);
    EXPECT_FALSE(addOverflow(kMax, kMin, r));
    EXPECT_EQ(r, -1);
    EXPECT_FALSE(addOverflow(kMin, kMax, r));
    EXPECT_EQ(r, -1);
    EXPECT_FALSE(addOverflow(kMin + 1, 1, r));
    EXPECT_EQ(r, kMin + 2);
    EXPECT_FALSE(addOverflow(kMax - 1, 1, r));
    EXPECT_EQ(r, kMax);
}

TEST(CheckedAdd, OverflowKeepsOut) {
    int64_t r = 12345;
    EXPECT_TRUE(addOverflow(kMax, 1, r));
    EXPECT_EQ(r, 12345) << "溢出时不能写 out";
    EXPECT_TRUE(addOverflow(kMax, kMax, r));
    EXPECT_EQ(r, 12345);
    EXPECT_TRUE(addOverflow(kMin, -1, r));
    EXPECT_EQ(r, 12345);
    EXPECT_TRUE(addOverflow(kMin, kMin, r));
    EXPECT_EQ(r, 12345);
}

TEST(CheckedSub, NormalCases) {
    int64_t r = 0;
    EXPECT_FALSE(subOverflow(0, 0, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(subOverflow(1, 2, r));
    EXPECT_EQ(r, -1);
    EXPECT_FALSE(subOverflow(-1, -2, r));
    EXPECT_EQ(r, 1);
}

TEST(CheckedSub, Boundaries) {
    int64_t r = 0;
    EXPECT_FALSE(subOverflow(kMin, 0, r));
    EXPECT_EQ(r, kMin);
    EXPECT_FALSE(subOverflow(kMin, kMin, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(subOverflow(kMax, kMax, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(subOverflow(kMin, -1, r));
    EXPECT_EQ(r, kMin + 1);
    EXPECT_FALSE(subOverflow(kMax, kMax - 1, r));
    EXPECT_EQ(r, 1);
}

TEST(CheckedSub, OverflowKeepsOut) {
    int64_t r = 7;
    EXPECT_TRUE(subOverflow(kMin, 1, r));
    EXPECT_EQ(r, 7);
    EXPECT_TRUE(subOverflow(kMax, -1, r));
    EXPECT_EQ(r, 7);
    EXPECT_TRUE(subOverflow(kMax, kMin, r));
    EXPECT_EQ(r, 7);
    EXPECT_TRUE(subOverflow(kMin, kMax, r));
    EXPECT_EQ(r, 7);
}

TEST(CheckedMul, ZeroAndOne) {
    int64_t r = 9;
    EXPECT_FALSE(mulOverflow(0, 0, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(mulOverflow(0, kMin, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(mulOverflow(kMax, 0, r));
    EXPECT_EQ(r, 0);
    EXPECT_FALSE(mulOverflow(1, kMin, r));
    EXPECT_EQ(r, kMin);
    EXPECT_FALSE(mulOverflow(kMin, 1, r));
    EXPECT_EQ(r, kMin);
    EXPECT_FALSE(mulOverflow(kMax, 1, r));
    EXPECT_EQ(r, kMax);
    EXPECT_FALSE(mulOverflow(-1, kMax, r));
    EXPECT_EQ(r, -kMax);
}

TEST(CheckedMul, BothSigns) {
    int64_t r = 0;
    EXPECT_FALSE(mulOverflow(2, 3, r));
    EXPECT_EQ(r, 6);
    EXPECT_FALSE(mulOverflow(-2, 3, r));
    EXPECT_EQ(r, -6);
    EXPECT_FALSE(mulOverflow(2, -3, r));
    EXPECT_EQ(r, -6);
    EXPECT_FALSE(mulOverflow(-2, -3, r));
    EXPECT_EQ(r, 6);
    EXPECT_FALSE(mulOverflow(-1, -1, r));
    EXPECT_EQ(r, 1);
}

TEST(CheckedMul, Boundaries) {
    int64_t r = 0;
    // 2^31 * 2^32 = 2^63：正数放不下，负数一侧刚好放下
    const int64_t p31 = (int64_t) 1 << 31, p32 = (int64_t) 1 << 32;
    EXPECT_TRUE(mulOverflow(p31, p32, r));
    EXPECT_FALSE(mulOverflow(-p31, p32, r));
    EXPECT_EQ(r, kMin);
    EXPECT_FALSE(mulOverflow(p31, -p32, r));
    EXPECT_EQ(r, kMin);
    EXPECT_TRUE(mulOverflow(-p31, -p32, r));

    // 2^31 * 2^31 = 2^62 两侧都放得下
    EXPECT_FALSE(mulOverflow(p31, p31, r));
    EXPECT_EQ(r, (int64_t) 1 << 62);
    EXPECT_FALSE(mulOverflow(-p31, -p31, r));
    EXPECT_EQ(r, (int64_t) 1 << 62);

    // floor(sqrt(2^63-1)) = 3037000499，再大 1 就溢出
    EXPECT_FALSE(mulOverflow(3037000499LL, 3037000499LL, r));
    EXPECT_EQ(r, 3037000499LL * 3037000499LL);
    EXPECT_FALSE(mulOverflow(-3037000499LL, 3037000499LL, r));
    EXPECT_EQ(r, -3037000499LL * 3037000499LL);
    EXPECT_TRUE(mulOverflow(3037000500LL, 3037000500LL, r));
    EXPECT_TRUE(mulOverflow(-3037000500LL, 3037000500LL, r));
}

TEST(CheckedMul, OverflowKeepsOut) {
    int64_t r = -11;
    EXPECT_TRUE(mulOverflow(-1, kMin, r));
    EXPECT_EQ(r, -11);
    EXPECT_TRUE(mulOverflow(kMin, -1, r));
    EXPECT_EQ(r, -11);
    EXPECT_TRUE(mulOverflow(kMax, 2, r));
    EXPECT_EQ(r, -11);
    EXPECT_TRUE(mulOverflow(kMax, kMax, r));
    EXPECT_EQ(r, -11);
    EXPECT_TRUE(mulOverflow(kMin, kMin, r));
    EXPECT_EQ(r, -11);
}
