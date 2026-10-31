/* RISC-V 启动垫片：真机上的 bootloader（OpenSBI/U-Boot）负责按启动协议
 * 设置 a0=hartid、a1=DTB 物理地址再跳进内核。这里用一个极小的 stub 顶替它，
 * 好处是内核可以照常以扁平 Image 的形式链接进来，dcs 的 --elf 加载器
 * 只认入口地址、不认单独的参数，因此参数必须在代码里设置而不是命令行传。
 *
 * 另外还得替 OpenSBI 干一件事：内核要求以 S 态启动，SBI 服务由 CPU 的
 * ecall 处理顶替，所以这里把异常/中断委派好，用 mret 从 M 态落到 S 态。
 * 三段布局：stub @0x80000000，DTB @0x80100000，Image @0x80200000。 */
    .section .text.boot, "ax"
    .globl _start
_start:
    li      a0, 0                   /* hartid */
    li      a1, 0x80100000          /* DTB 物理地址，内核在 setup 阶段解析 */

    /* 异常全部委派给 S 态，唯独 ECALL_S 例外——
     * 内核正是靠 ecall 进 M 态调用 SBI 的，委派出去就会自己处理自己 */
    li      t0, 0xFFFF                  /* 16 个异常号先全置上 */
    li      t1, 1 << 9                  /* CAUSE_ECALL_S */
    not     t1, t1
    and     t0, t0, t1
    csrw    medeleg, t0

    /* 软件/定时器/外部中断都交给 S 态 */
    li      t0, (1 << 1) | (1 << 5) | (1 << 9)
    csrw    mideleg, t0

    /* mstatus: MPP=S(01) 让 mret 落到 S 态，MPIE=1 给内核一个已开中断的起点 */
    li      t0, (1 << 11) | (1 << 7)
    csrw    mstatus, t0
    li      t0, 0x80200000          /* Linux Image 的物理入口 */
    csrw    mepc, t0
    mret
