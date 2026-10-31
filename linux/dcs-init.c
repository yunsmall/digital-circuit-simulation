// 最小 init：不依赖 libc，用内核源码自带的 nolibc 静态编译。
// 打印一句话后死循环，让仿真器能稳定停在用户态而不是 panic。
#include "nolibc.h"

static const char banner[] = "\n[dcs] Hello from Linux userspace!\n[dcs] init reached, kernel booted.\n";

int main(void) {
    write(1, banner, sizeof(banner) - 1);
    for (;;)
        __asm__ volatile("wfi");
    return 0;
}
