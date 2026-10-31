# 最小 init：直接走 Linux syscall，不依赖任何 libc。
# 自己 open("/dev/console") 再往那个 fd 写——内核给的 fd 1 可能是 /dev/null，
# 那样 write 也会返回成功但数据被丢弃，必须自己拿一个确定的 tty 才能验证串口通路。
    .section .text
    .globl _start
_start:
    li      a0, -100               # AT_FDCWD
    la      a1, devconsole
    li      a2, 1                  # O_WRONLY
    li      a3, 0
    li      a7, 56                 # SYS_openat
    ecall
    mv      s0, a0                 # s0 = fd（失败为负）

    mv      a0, s0                 # 往 open 出来的 fd 写
    la      a1, banner
    li      a2, banner_end - banner
    li      a7, 64                 # SYS_write
    ecall
    mv      s1, a0                 # s1 = write 返回值

    # open 失败时退出码用 0xF0 区分；否则把 write 的返回值当退出码
    mv      a0, s0
    bgez    a0, 1f
    li      a0, 0xF0
    j       2f
1:  mv      a0, s1
2:  li      a7, 93                 # SYS_exit
    ecall

3:  j       3b

    .section .rodata
devconsole:
    .asciz "/dev/console"
banner:
    .ascii "\n[dcs] Hello from Linux userspace!\n"
    .ascii "[dcs] init reached, kernel booted.\n"
banner_end:
