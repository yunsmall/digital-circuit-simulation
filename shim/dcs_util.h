// 数字电路仿真 C 运行时工具 — 不依赖系统头文件
#ifndef DCS_SHIM_UTIL_H
#define DCS_SHIM_UTIL_H

// 生成的元件函数统一用这个前缀：static + 强制内联。
// 每个元件函数每拍都要调用一次，函数序言/返回在几十万拍的仿真里是实打实的开销；
// 强制内联把它们全部展开进 circuit_tick，省掉这部分。TCC 本身不做内联优化，
// 关键字放它身上不产生额外代价
#if defined(__TINYC__)
#define DCS_INLINE static inline
#elif defined(_MSC_VER) && !defined(__clang__)
#define DCS_INLINE static __forceinline
#else
#define DCS_INLINE static inline __attribute__((always_inline))
#endif

#ifdef __clang__
// LLVM/Clang 后端：直接用编译器内置函数，编译器会按内联展开
static inline void *dcs_memcpy(void *dst, const void *src, unsigned long long n) {
    return __builtin_memcpy(dst, src, n);
}
static inline void *dcs_memset(void *dst, int c, unsigned long long n) {
    return __builtin_memset(dst, c, n);
}
#elif defined(__TINYC__)
// TCC 后端：TCC 不做内联和优化，手写循环会比 libc 的向量化实现慢；
// 这两个符号由 JitBackend 用 tcc_add_symbol 注册成 libc 的 memcpy/memset
extern void *dcs_memcpy(void *, const void *, unsigned long long);
extern void *dcs_memset(void *, int, unsigned long long);
#else
// 其它编译器：标准 C 通用实现
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

#endif // DCS_SHIM_UTIL_H
