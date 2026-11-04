// 有符号 64 位整数的溢出检查
//
// MSVC 不提供 __builtin_*_overflow，而模板表达式在算参数时必须拒绝溢出
// （溢出后的值会静默写坏电路参数），所以这里用可移植写法：加减法按边界
// 比较，乘法先转成绝对值做无符号乘法，再判结果是否落回 int64_t 的范围。
#pragma once
#include <cstdint>

namespace dsc::checked {

// 成功返回 false 并写 out；溢出返回 true，out 保持原值
inline bool addOverflow(int64_t a, int64_t b, int64_t &out) {
    if (b > 0) {
        if (a > INT64_MAX - b)
            return true;
    } else if (a < INT64_MIN - b) {
        return true;
    }
    out = a + b;
    return false;
}

inline bool subOverflow(int64_t a, int64_t b, int64_t &out) {
    if (b > 0) {
        if (a < INT64_MIN + b)
            return true;
    } else if (a > INT64_MAX + b) {
        return true;
    }
    out = a - b;
    return false;
}

// INT64_MIN 的绝对值是 2^63，std::abs 在这里是未定义行为
inline uint64_t absU64(int64_t v) {
    return v < 0 ? ~(uint64_t) v + 1 : (uint64_t) v;
}

inline bool mulOverflow(int64_t a, int64_t b, int64_t &out) {
    const uint64_t ua = absU64(a), ub = absU64(b);
    if (ua != 0 && ub > UINT64_MAX / ua)
        return true; // 无符号乘积都放不下，肯定也超出 int64
    const uint64_t p = ua * ub;
    // 负数一侧能到 INT64_MIN（绝对值 2^63），正数一侧只能到 INT64_MAX
    const bool neg = (a < 0) != (b < 0);
    const uint64_t limit = neg ? (uint64_t) INT64_MAX + 1 : (uint64_t) INT64_MAX;
    if (p > limit)
        return true;
    out = neg ? (int64_t) (~p + 1) : (int64_t) p;
    return false;
}

} // namespace dsc::checked
