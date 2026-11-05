// 数字电路仿真 C 运行时工具 — 不依赖系统头文件
#ifndef DCS_SHIM_UTIL_H
#define DCS_SHIM_UTIL_H

// 生成的元件函数一律是普通 static 函数，不强制展开：LLVM 后端不开内联 pass
// （实测把元件函数全部内联进 circuit_tick 后指令缓存被挤爆，吞吐反而下降
// 5~15%，JIT 编译时间还翻倍），元件之间保持普通函数调用。
// 只有每拍被调几十次的极热小包装才用 DCS_FORCE_INLINE。

// 每拍被调几十次的小包装（线网搬运）：调用开销会盖过函数体，必须展开
#if defined(__TINYC__)
#define DCS_FORCE_INLINE static inline
#elif defined(_MSC_VER) && !defined(__clang__)
#define DCS_FORCE_INLINE static __forceinline
#else
#define DCS_FORCE_INLINE static inline __attribute__((always_inline))
#endif

// 65~128 位的线网值。不用 __int128：TCC 不支持，而宽线网只需要按字节搬运
typedef struct {
    unsigned long long lo;
    unsigned long long hi;
} dcs_u128;

// 生成的代码按 16 字节整槽搬运 dcs_u128，并假设首成员地址就是结构体地址。
// 用 typedef 而不是 _Static_assert：TCC 对后者支持不全，这里只要求能编过
typedef char dcs_u128_must_be_16_bytes[(sizeof(dcs_u128) == 16) ? 1 : -1];

#ifdef __clang__
// LLVM/Clang 后端：直接用编译器内置函数，编译器会按内联展开
DCS_FORCE_INLINE void *dcs_memcpy(void *dst, const void *src, unsigned long long n) {
    return __builtin_memcpy(dst, src, n);
}
DCS_FORCE_INLINE void *dcs_memset(void *dst, int c, unsigned long long n) {
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
// 这里必须先把指针转成 unsigned int*：调用点传进来的可能是 char*（状态结构体里
// 的偏移指针），直接 __atomic_load_n(char*, ...) 只会读 1 个字节，高位全丢
#define dcs_load_acq(p) __atomic_load_n((unsigned int *) (p), __ATOMIC_ACQUIRE)
#define dcs_store_rel(p, v) __atomic_store_n((unsigned int *) (p), (unsigned int) (v), __ATOMIC_RELEASE)
#endif

#endif // DCS_SHIM_UTIL_H
