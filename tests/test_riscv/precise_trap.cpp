// 精确异常：trap 时 PC 与寄存器必须停在"该指令未执行"的状态
#include "platform.h"

// 这是 Linux 能跑的前提：page fault 后 S 态处理程序修复页表、重新执行该指令，
// 任何"半执行"（store 已写、rd 已改、PC 已前进）都会让重试出错。

TEST(PreciseTrap, IllegalInstructionKeepsRegisters) {
    PrivTestFixture f;
    f.loadProgram({
        0x02000093, // 0x00: addi x1, x0, 0x20
        0x30509073, // 0x04: csrrw x0, mtvec, x1
        0x00700093, // 0x08: addi x1, x0, 7     rd 的旧值
        0x00000000, // 0x0C: 全零字 = 非法指令
        0x00000013, // 0x10: nop
        0x00000013, // 0x14: nop
        0x00000013, // 0x18: nop
        0x00000013, // 0x1C: nop
        0x342021F3, // 0x20: csrrs x3, mcause, x0   ← mtvec 指向这里
        0x34102273, // 0x24: csrrs x4, mepc, x0
        0x0000006F, // 0x28: jal x0, 0
    });
    f.compile();
    f.reset();
    f.runCycles(100);
    EXPECT_EQ(f.cpu->reg(3), 2u) << "非法指令的 mcause 应为 2";
    EXPECT_EQ(f.cpu->reg(1), 7u) << "非法指令不得修改任何寄存器";
    EXPECT_EQ(f.cpu->reg(4), 0x0Cu) << "mepc 应指向出错指令本身";
}

TEST(PreciseTrap, EcallKeepsPcAtInstruction) {
    PrivTestFixture f;
    f.loadProgram({
        0x02000093, // 0x00: addi x1, x0, 0x20
        0x30509073, // 0x04: csrrw x0, mtvec, x1
        0x00000013, // 0x08: nop
        0x00000073, // 0x0C: ecall（M 态）
        0x00000013, // 0x10: nop
        0x00000013, // 0x14: nop
        0x00000013, // 0x18: nop
        0x00000013, // 0x1C: nop
        0x342021F3, // 0x20: csrrs x3, mcause, x0
        0x34102273, // 0x24: csrrs x4, mepc, x0
        0x0000006F, // 0x28: jal x0, 0
    });
    f.compile();
    f.reset();
    f.runCycles(100);
    EXPECT_EQ(f.cpu->reg(3), 11u) << "M 态 ecall 的 cause 是 11";
    EXPECT_EQ(f.cpu->reg(4), 0x0Cu) << "mepc 应指向 ecall 本身";
}
