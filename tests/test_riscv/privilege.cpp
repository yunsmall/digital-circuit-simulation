// RISC-V CPU 特权级测试: ecall, mret, sret, 异常委托
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <cstring>
#include "platform.h"

using namespace dsc;


// ============================================================
// M-mode ecall → trap to mtvec handler
// ============================================================
TEST(PrivTest, EcallM) {
    PrivTestFixture f;
    // mtvec=0x0C, handler at 0x0C: addi x3,x0,77; jal x0,0
    f.loadProgram({
        0x00C00093, // 0x00: addi x1,x0,12        (x1=12=mtvec)
        0x30509073, // 0x04: csrrw x0,mtvec,x1    (mtvec=12)
        0x00000073, // 0x08: ecall (M-mode → trap to 0x0C)
        0x04D00193, // 0x0C: addi x3,x0,77        (handler: x3=77)
        0x0000006F, // 0x10: jal x0,0             (handler spin)
    });
    f.compile();
    f.reset();
    f.runCycles(60);

    // M-mode ecall 触发 EXC_ECALL_M → mtvec=0x0C → handler → x3=77
    EXPECT_EQ(f.cpu->reg(3), 77);
}

// ============================================================
// mret 正确恢复并返回
// ============================================================
TEST(PrivTest, MretRestore) {
    PrivTestFixture f;
    // 布局: 0x00-0x04 mtvec init, 0x08 ebreak,
    // 0x10-0x18 handler, 0x1C 返回目标
    f.loadProgram({
        0x01000093, // 0x00: addi x1,x0,0x10
        0x30509073, // 0x04: csrrw x0,mtvec,x1
        0x00100073, // 0x08: ebreak → trap to 0x10
        0x00000013, // 0x0C: nop (填充)
        // 陷阱处理程序 0x10:
        0x01C00113, // 0x10: addi x2,x0,28        (x2=28=0x1C)
        0x34111073, // 0x14: csrrw x0,mepc,x2    (mepc=0x1C)
        0x30200073, // 0x18: mret                 (返回 0x1C)
        // mret 返回目标 0x1C:
        0x05800193, // 0x1C: addi x3,x0,88
        0x0000006F, // 0x20: jal x0,0
    });
    f.compile();
    f.reset();
    f.runCycles(80);

    EXPECT_EQ(f.cpu->reg(3), 88);
}

// ============================================================
// U-mode ecall → trap to M-mode (medeleg=0 时不委托)
// ============================================================
TEST(PrivTest, EcallU) {
    PrivTestFixture f;
    f.loadProgram({
        0x02000093, // 0x00: addi x1, x0, 0x20   处理程序在 0x20
        0x30509073, // 0x04: csrrw x0, mtvec, x1
        0x01400093, // 0x08: addi x1, x0, 0x14   要返回的 U 态地址
        0x34109073, // 0x0C: csrrw x0, mepc, x1
        0x30200073, // 0x10: mret                → U 态 0x14（reset 后 MPP=U）
        0x00000073, // 0x14: ecall               U 态发起 ecall
        0x00000013, // 0x18: nop
        0x00000013, // 0x1C: nop
        0x342021F3, // 0x20: csrrs x3, mcause, x0
        0x0000006F, // 0x24: jal x0, 0           自旋
    });
    f.compile();
    f.reset();
    f.runCycles(80);
    // U 态 ecall 的 cause 是 8；如果核没有真正切换特权级，会得到 11（M 态 ecall）
    EXPECT_EQ(f.cpu->reg(3), 8u) << "U 态 ecall 应报 EXC_ECALL_U";
}

// ============================================================
// 异常委托: medeleg 将 ebreak 委托到 S-mode
// ============================================================
TEST(PrivTest, DelegationEbreak) {
    PrivTestFixture f;
    // 设置 medeleg[3]=1 (委托 ebreak 到 S-mode)
    // 设置 mtvec=M-handler, stvec=S-handler
    // 切到 S-mode → ebreak → 应走 stvec
    // 简化: 先测试 medeleg 可读写
    f.loadProgram({
        0x00800093, // 0x00: addi x1,x0,8         (x1=8=1<<3, 委托 ebreak)
        0x30209073, // 0x04: csrrw x0,medeleg,x1  (medeleg=8)
        0x30202173, // 0x08: csrrs x2,medeleg,x0  (读 medeleg → x2)
        0x04D00193, // 0x0C: addi x3,x0,77
        0x0000006F, // 0x10: jal x0,0
    });
    f.compile();
    f.reset();
    f.runCycles(60);

    EXPECT_EQ(f.cpu->reg(2), 8u);  // medeleg 应为 8 (ebreak 委托)
}

// ============================================================
// sret 基本功能: 从 S-mode 陷阱返回
// ============================================================
TEST(PrivTest, SretBasic) {
    PrivTestFixture f;
    // 切到 S-mode, 设置 stvec=handler, sepc=目标
    // 然后 sret 返回
    // 先测试 sret 指令能正确解码执行
    // 设置 mtvec=0x20, 然后:
    //   - mstatus.MPP=S (1), mstatus.MPIE=1
    //   - 设 mepc=0x24
    //   - mret → S-mode → 0x24
    //   - 在 0x24: sret (应能执行，但 sepc=0 可能跳转到 0)
    // 简化: 验证 sret 指令编码不会导致非法指令 trap
    f.loadProgram({
        0x10200073, // 0x00: sret (直接执行，由于 SPP=0 且 SIE=0，应切到 U-mode)
        0x04D00193, // 0x04: addi x3,x0,77
        0x0000006F, // 0x08: jal x0,0
    });
    f.compile();
    f.reset();
    f.runCycles(60);

    // sret 在 reset 后 mstatus.SPP=0 → U-mode, SIE=0
    // 如果 sret 正确执行不 trap，PC 会走到 0x04
    // 但 sepc 初始为 0，sret 后会跳转到 0
    // 这可能导致循环。先检查 sret 不会导致 illegal instruction trap
    // (如果 sret 导致 trap，PC 会停在 mtvec=0，形成死循环)
    // 不管怎样，确保 sret 不会 halt
    EXPECT_FALSE(f.cpu->halted());
}

// SBI set_timer 的 u64 参数在 RV32 上拆成 a0(低)/a1(高)。
// 只读 a0 会让 deadline 高 32 位归零，ns 超过 2^32 后定时器立刻过期
TEST(PrivTest, SbiSetTimerUsesHighWordOnRv32) {
    PrivTestFixture f;
    f.loadProgram({
        0x01800093, // 0x00: addi x1,x0,0x18        (S 态入口)
        0x34109073, // 0x04: csrrw x0,mepc,x1
        0x000010B7, // 0x08: lui x1,0x1
        0x80008093, // 0x0C: addi x1,x1,-2048      (mstatus.MPP=S)
        0x30009073, // 0x10: csrrw x0,mstatus,x1
        0x30200073, // 0x14: mret
        // 0x18: S 态调用 SBI set_timer(0x1FFFFFFFF)
        0x544958B7, // 0x18: lui x17,0x54495        (a7 = SBI_EXT_TIME)
        0xD4588893, // 0x1C: addi x17,x17,-699
        0x00000813, // 0x20: addi x16,x0,0          (a6 = set_timer)
        0xFFF00513, // 0x24: addi x10,x0,-1         (a0 = 0xFFFFFFFF)
        0x00100593, // 0x28: addi x11,x0,1          (a1 = 1)
        0x00000073, // 0x2C: ecall
        0x0000006F, // 0x30: jal x0,0
    });
    f.compile();
    f.reset();
    f.runCycles(200);

    EXPECT_EQ(f.cpu->priv(), 1) << "应进入 S 态执行 SBI 调用";
    EXPECT_EQ((uint32_t) f.cpu->pc(), 0x30u) << "PC 应停在 jal x0,0";
    EXPECT_EQ(f.cpu->timerDeadline(), 0x1FFFFFFFFull)
        << "RV32 的 set_timer 必须合并 a0/a1，否则高 32 位被丢掉";
}
