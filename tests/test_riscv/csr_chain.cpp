// CSR 写路径不能 fall through：每个 case 必须自己 break。
// 曾经把 break; 写进行尾注释，结果写 mstatus 会顺带覆盖 medeleg。
#include <dcs/Circuit.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>
#include <gtest/gtest.h>

#include "platform.h"

using namespace dsc;

namespace {
// csrrw x0, csr, rs1 的编码
constexpr uint32_t csrrw(int rd, int csr, int rs1) {
    return ((uint32_t)(csr & 0xFFF) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | (1u << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x73u;
}
constexpr uint32_t addi(int rd, int rs1, int imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | (0u << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x13u;
}
constexpr uint32_t jal(int rd, int off) {
    const uint32_t u = (uint32_t)off;
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1u) << 20)
         | (((u >> 12) & 0xFF) << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x6Fu;
}
} // namespace

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
