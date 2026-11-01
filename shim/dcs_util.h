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

// 65~128 位的线网值。不用 __int128：TCC 不支持，而宽线网只需要按字节搬运
typedef struct {
    unsigned long long lo;
    unsigned long long hi;
} dcs_u128;

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

// 队列头尾可能被宿主后端线程和 JIT 同时访问（网络收发的 SPSC 环）。
// Clang 直接内联原子指令；TCC 的 JIT 是 -nostdlib，和 memset 一样由宿主注册函数
#if defined(__TINYC__)
extern unsigned int dcs_atomic_load_acq(const void *p);
extern void dcs_atomic_store_rel(void *p, unsigned int v);
#define dcs_load_acq(p) dcs_atomic_load_acq(p)
#define dcs_store_rel(p, v) dcs_atomic_store_rel(p, v)
#else
#define dcs_load_acq(p) __atomic_load_n((unsigned int *) (p), __ATOMIC_ACQUIRE)
#define dcs_store_rel(p, v) __atomic_store_n((unsigned int *) (p), (unsigned int) (v), __ATOMIC_RELEASE)
#endif

#endif // DCS_SHIM_UTIL_H
