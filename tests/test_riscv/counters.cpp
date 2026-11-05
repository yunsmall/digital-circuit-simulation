#include "platform.h"

#include <vector>

// cycle/instret 必须是真实递增的：软件靠 rdcycle 做性能测量和超时检测
TEST(CounterCsr, CycleAndInstretAdvance) {
    PrivTestFixture f;
    f.loadProgram({
        0xC00022F3, // 0x00: rdcycle x5     先读一次
        0xC0202373, // 0x04: rdinstret x6
        0x00000013, // 0x08: nop
        0x00000013, // 0x0C: nop
        0x00000013, // 0x10: nop
        0xC0002A73, // 0x14: rdcycle x20    再读一次，不覆盖 x5
        0xC0202AF3, // 0x18: rdinstret x21
        0x0000006F, // 0x1C: jal x0, 0      自旋
    });
    f.compile();
    f.reset();
    f.runCycles(60);
    EXPECT_GT(f.cpu->reg(20), f.cpu->reg(5)) << "cycle 应随时钟递增";
    EXPECT_GT(f.cpu->reg(21), f.cpu->reg(6)) << "instret 应随指令递增";
    EXPECT_EQ(f.cpu->reg(3), 0u) << "不应有异常";
}


// Zicntr：以同步异常结束的指令（ECALL/EBREAK 等）不算 retire，不能递增 instret。
// 曾经实现把走到行尾的 trap 指令也算一条，读 minstret 的差值会多 1
TEST(CounterCsr, EcallDoesNotIncrementInstret) {
    PrivTestFixture f;
    std::vector<uint32_t> prog(70, 0x00000013u);   // nop 填充
    prog[0] = 0x10000093u;      // addi x1, x0, 0x100
    prog[1] = 0x30509073u;      // csrrw x0, mtvec, x1
    prog[2] = 0xC02022F3u;      // rdinstret x5
    prog[3] = 0x00000073u;      // ecall（进 0x100 的陷阱入口）
    prog[4] = 0x0000006Fu;      // 没进陷阱时自旋
    prog[64] = 0xC0202373u;     // 0x100: rdinstret x6
    prog[65] = 0x0000006Fu;

    f.loadProgram(prog);
    f.compile();
    f.reset();
    f.runCycles(100);
    EXPECT_EQ(f.cpu->reg(6) - f.cpu->reg(5), 1u)
        << "ecall 本身不应递增 instret（两次读取之间只有它之前的 rdinstret 那条指令）";
}
