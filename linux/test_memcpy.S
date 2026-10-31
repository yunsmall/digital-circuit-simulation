/* 复现内核 __memcpy 的完整路径：len >= 128 会先走 16*SZREG 的块复制，
 * 再走尾部逐字节/4 字节循环。之前只测过 29 字节（纯尾部），
 * 而内核里 task_struct/pt_regs 的复制都是大块，这条路径必须单独验证。 */
    .section .text
    .globl _start
_start:
    la      a1, src
    la      t3, dst
    addi    t4, t3, 200             /* 复制 200 字节 */
    beq     t3, t4, 2f
1:
    lb      t1, 0(a1)
    addi    a1, a1, 1
    addi    t3, t3, 1
    sb      t1, -1(t3)
    bne     t3, t4, 1b
2:  j       2b

    .section .data
    .balign 8
src:
    .rept   25
    .ascii  "01234567"
    .endr
    .byte   0
    .balign 8
dst:
    .space  320
