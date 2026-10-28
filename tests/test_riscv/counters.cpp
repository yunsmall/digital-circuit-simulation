#include "platform.h"

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

