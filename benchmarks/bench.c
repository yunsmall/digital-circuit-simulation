// 基准测试负载。每个场景用 -DBENCH=n 单独编译成一个 ELF，
// 由 benchmarks/benchmark.cpp 加载并计时。
// 循环都带数据依赖（下一轮的输入来自上一轮），避免 -O2 把整段算成常量。

volatile unsigned long bench_sink;

#define OUTER 4000
#define INNER 1000

#if BENCH == 0
// 整数 ALU：add/mul/xor/移位混在一起
void bench_main(void) {
    unsigned long x = 1;
    for (int i = 0; i < OUTER; i++) {
        for (int j = 0; j < INNER; j++) {
            x = x * 1664525UL + 1013904223UL;
            x ^= x >> 13;
            x += (unsigned long) j;
        }
    }
    bench_sink = x;
}

#elif BENCH == 1
// 访存：4KB 数组反复读改写
static unsigned long buf[512];
void bench_main(void) {
    unsigned long acc = 0;
    for (int i = 0; i < 512; i++) buf[i] = (unsigned long) i;
    for (int r = 0; r < 300; r++) {
        for (int i = 0; i < 512; i++) buf[i] = buf[i] * 3 + (unsigned long) i;
        for (int i = 0; i < 512; i++) acc += buf[i];
    }
    bench_sink = acc;
}

#elif BENCH == 2
// 分支：每轮结果决定下一条走哪边，分支预测友好度低
void bench_main(void) {
    unsigned long x = 12345;
    for (int i = 0; i < OUTER; i++) {
        for (int j = 0; j < INNER; j++) {
            if (x & 1)
                x = x * 3 + 1;
            else
                x >>= 1;
            if (x > 0x8000000000000000UL)
                x = 12345;
        }
    }
    bench_sink = x;
}

#elif BENCH == 3
// 乘除：DIV/REM 是 RISC-V 里最贵的整数运算
void bench_main(void) {
    unsigned long x = 12345;
    for (int i = 0; i < OUTER; i++) {
        for (int j = 0; j < INNER; j++) {
            x = x * 6364136223846793005UL + 1442695040888963407UL;
            x = x / 7 + x % 13;
        }
    }
    bench_sink = x;
}

#elif BENCH == 4
// 双精度浮点：FADD/FMUL/FDIV 各占一部分
void bench_main(void) {
    double d = 1.0;
    for (int i = 0; i < OUTER; i++) {
        for (int j = 0; j < INNER; j++) {
            d = d * 1.0000001 + 0.5;
            if (d > 1e6) d = 1.0;
            d = d / 1.0000002;
        }
    }
    bench_sink = (unsigned long) d;
}

#elif BENCH == 5
// 混合：整数 + 访存 + 分支，接近真实程序的比例
static unsigned long tab[256];
void bench_main(void) {
    unsigned long x = 1, acc = 0;
    for (int r = 0; r < 2000; r++) {
        for (int i = 0; i < 256; i++) {
            unsigned long v = tab[i] + x;
            if (v & 1) v = v * 3 + 1;
            else v += v >> 1;
            tab[i] = v;
            acc ^= v;
        }
        x = acc;
    }
    bench_sink = x;
}

#endif
