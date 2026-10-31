/* 验证 LR/SC 的 cmpxchg 语义：内核的自旋锁就是这么实现的。
 * 如果 SC 永远失败（保留集被误清），锁永远拿不到，表现为卡在 do_raw_spin_lock。
 * 预期：把 a2(旧值) 换成 a3(新值)，成功返回 0。 */
    .section .text
    .globl _start
_start:
    la      a0, target
    li      a1, 0               /* 期望旧值 */
    li      a2, 1               /* 新值 */
    mv      a3, a0
1:  lr.w    a4, (a3)            /* a4 = *target */
    bne     a4, a1, 3f          /* 旧值不符 -> 退出 */
    sc.w    a5, a2, (a3)        /* 尝试写入 */
    bnez    a5, 1b              /* SC 失败 -> 重试 */
3:  sw      a5, 0x20(zero)      /* 把 SC 结果存到 0x20 方便观察 */
    la      t0, done
    li      t1, 1
    sw      t1, 0(t0)
4:  j       4b

    .section .data
    .balign 8
target:
    .word   0
    .balign 8
done:
    .word   0
