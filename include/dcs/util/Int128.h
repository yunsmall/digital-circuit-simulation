// 128 位中间结果的跨平台封装
//
// RISC-V 的 MULH/MULHSU/MULHU、Zbc 的 clmul、非对齐访存的字节拼接都需要
// 128 位中间结果。GCC/Clang 有 __int128，MSVC 没有（x64 下也不支持），
// 所以这里把用得到的几种运算收成一组函数：MSVC 走 <intrin.h> 的内建指令，
// 其余编译器走 __int128。两边的结果完全等价。
#pragma once
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace dsc {

// 无符号 64×64 → 128 位乘法：hi 接收高 64 位，返回低 64 位
inline uint64_t mul64x64(uint64_t a, uint64_t b, uint64_t &hi) {
#if defined(_MSC_VER) && !defined(__clang__)
    return _umul128(a, b, &hi);
#else
    const unsigned __int128 p = (unsigned __int128) a * b;
    hi = (uint64_t) (p >> 64);
    return (uint64_t) p;
#endif
}

// 无符号乘的高 64 位（MULHU）
inline uint64_t mulhu64(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && !defined(__clang__)
    return __umulh(a, b);
#else
    uint64_t hi = 0;
    mul64x64(a, b, hi);
    return hi;
#endif
}

// 有符号乘的高 64 位（MULH）
inline uint64_t mulh64(int64_t a, int64_t b) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (uint64_t) __mulh(a, b);
#else
    return (uint64_t) (((__int128) a * (__int128) b) >> 64);
#endif
}

// 有符号 × 无符号 乘的高 64 位（MULHSU）
inline uint64_t mulhsu64(int64_t a, uint64_t b) {
    // 先按无符号乘，再修正：负数 a 的 128 位补码是 2^64 + a，
    // 它与 b 的乘积高半比按真值算多出 b，减掉即可。这样两边共用 __umulh，
    // 不用为 MSVC 单独拼一套有符号×无符号的移位加法
    uint64_t hi = mulhu64((uint64_t) a, b);
    if (a < 0)
        hi -= b;
    return hi;
}

// 无符号 128 位累加器：只提供 RISC-V 核用到的那几种逐位操作。
// 用两个 uint64_t 表示，布局与 __int128 相同（低字在前），
// CpuState 里跨 JIT/C++ 共享时不会出现对齐差异
struct U128 {
    uint64_t lo = 0, hi = 0;

    void set(uint64_t v) {
        lo = v;
        hi = 0;
    }

    // acc |= (uint64_t)v << b，b 取值 0~127
    void or_shifted(uint64_t v, int b) {
        if (b < 0 || b >= 128)
            return;
        if (b >= 64) {
            hi |= v << (b - 64);
            return;
        }
        if (b == 0) {
            lo |= v;
            return;
        }
        lo |= v << b;
        hi |= v >> (64 - b);
    }

    // acc ^= (uint64_t)v << b，b 取值 0~127。
    // CLMUL 必须用异或：不同 b 的乘积项会落到同一批 bit 上，
    // 用或的话重复位置消不掉，结果就错了
    void xor_shifted(uint64_t v, int b) {
        if (b < 0 || b >= 128)
            return;
        if (b >= 64) {
            hi ^= v << (b - 64);
            return;
        }
        if (b == 0) {
            lo ^= v;
            return;
        }
        lo ^= v << b;
        hi ^= v >> (64 - b);
    }

    // (acc >> s) 的低 64 位，s 取值 0~127；s >= 128 返回 0
    [[nodiscard]] uint64_t shift_right_lo(int s) const {
        if (s <= 0)
            return lo;
        if (s >= 128)
            return 0;
        if (s >= 64)
            return hi >> (s - 64);
        return (lo >> s) | (hi << (64 - s));
    }
};

} // namespace dsc
