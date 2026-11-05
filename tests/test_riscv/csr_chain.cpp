// CSR 写路径不能 fall through：每个 case 必须自己 break。
// 曾经把 break; 写进行尾注释，结果写 mstatus 会顺带覆盖 medeleg。
#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <gtest/gtest.h>

#include "enc.h"
#include "platform.h"

using namespace dsc;
using namespace rv32enc;

// 写 mstatus 之后 medeleg 必须保持 0：两者在 switch 里相邻，
// 少一个 break 就会把 mstatus 的值漏进 medeleg
TEST(CsrChainTest, MstatusWriteDoesNotClobberMedeleg) {
    PrivTestFixture f;
    f.loadProgram({
        addi(7, 0, 0x8),        // x7 = MIE
        csrrw(0, 0x300, 7),     // mstatus = 0x8（顺带把 MPIE/MPP 清掉）
        csrrw(8, 0x302, 0),     // x8 = mdeleg 的旧值
        jal(0, 0),              // 自旋
    });
    f.compile();
    f.reset();
    f.runCycles(40);

    // medeleg 若被 mstatus 的值覆盖，x8 会看到 0x8 而不是 0
    EXPECT_EQ(f.cpu->reg(8), 0u) << "写 mstatus 漏到了 mdeleg";
}

// mstatus 的写入必须真的生效，不能被后面的 case 覆盖或者吞掉
TEST(CsrChainTest, MstatusWriteTakesEffect) {
    PrivTestFixture f;
    f.loadProgram({
        addi(7, 0, 0x8),        // x7 = MIE
        csrrw(0, 0x300, 7),     // mstatus = 0x8
        csrrw(8, 0x300, 0),     // x8 = 读回 mstatus（csrrw rd=x0 的写是覆盖）
        jal(0, 0),
    });
    f.compile();
    f.reset();
    f.runCycles(40);
    EXPECT_EQ(f.cpu->reg(8) & 0x8u, 0x8u) << "mstatus 的 MIE 没写进去";
}

// 规范：mcause 的 14 号异常是 Reserved，medeleg[14] 必须只读零。
// 允许写进去会让软件以为某个不存在的异常已委托
TEST(CsrChainTest, MedelegReservedBit14IsReadOnlyZero) {
    PrivTestFixture f;
    f.loadProgram({
        lui(7, 0x4),            // x7 = 0x4000（bit14）
        addi(7, 7, 0x8),        // 再加上 ebreak 的委托位
        csrrw(0, 0x302, 7),     // medeleg = 0x4008
        csrrs(8, 0x302, 0),     // x8 = 读回值
        jal(0, 0),
    });
    f.compile();
    f.reset();
    f.runCycles(40);
    EXPECT_EQ(f.cpu->reg(8), 8u) << "medeleg 的 bit14 应只读零，正常的 bit3 必须保留";
}
