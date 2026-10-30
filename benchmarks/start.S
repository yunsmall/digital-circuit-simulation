// 裸机入口：设置栈指针、打开浮点状态，跑完负载后写 tohost 表示结束。
// benchmark.cpp 会一直跑到 tohost 非零或达到拍数上限。
.section .text.init
.globl _start
_start:
    la      sp, _end
    li      t0, 0x6000          // mstatus.FS = Dirty，否则浮点指令按非法指令 trap
    csrs    mstatus, t0
    call    bench_main
    la      t0, tohost
    li      t1, 1
    sw      t1, 0(t0)           // 写 tohost：低 32 位写 1 表示正常结束
1:  j       1b
