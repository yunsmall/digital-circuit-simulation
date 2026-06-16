// 数字电路仿真 C 运行时工具 — 不依赖系统头文件
#pragma once

#ifdef __clang__
// LLVM/Clang 后端：使用编译器内置函数
static inline void *dcs_memcpy(void *dst, const void *src, unsigned long long n) {
    return __builtin_memcpy(dst, src, n);
}
static inline void *dcs_memset(void *dst, int c, unsigned long long n) {
    return __builtin_memset(dst, c, n);
}
#else
// 通用实现（标准 C，适用 TCC 等任何编译器）
static inline void *dcs_memcpy(void *dst, const void *src, unsigned long long n) {
    unsigned char *d = (unsigned char *) dst;
    const unsigned char *s = (const unsigned char *) src;
    while (n >= 8) {
        *(unsigned long long *) d = *(const unsigned long long *) s;
        d += 8;
        s += 8;
        n -= 8;
    }
    if (n >= 4) {
        *(unsigned *) d = *(const unsigned *) s;
        d += 4;
        s += 4;
        n -= 4;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}
static inline void *dcs_memset(void *dst, int c, unsigned long long n) {
    unsigned char *d = (unsigned char *) dst;
    unsigned long long v = (unsigned char) c;
    v = (v << 8) | v;
    v = (v << 16) | v;
    v = (v << 32) | v;
    while (n >= 8) {
        *(unsigned long long *) d = v;
        d += 8;
        n -= 8;
    }
    while (n--)
        *d++ = (unsigned char) c;
    return dst;
}
#endif
