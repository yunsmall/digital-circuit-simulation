// RV32 的边缘场景：官方测试覆盖不到、或者覆盖了也说不清行为的地方。
#include <gtest/gtest.h>

#include "platform.h"

namespace {

constexpr uint32_t i_type(int opcode, int rd, int f3, int rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | ((rs1 & 0x1F) << 15)
         | ((f3 & 7) << 12) | ((rd & 0x1F) << 7) | (opcode & 0x7F);
}
constexpr uint32_t addi_(int rd, int rs1, int32_t imm) { return i_type(0x13, rd, 0, rs1, imm); }
constexpr uint32_t lui_(int rd, uint32_t imm20) { return (imm20 << 12) | ((rd & 0x1F) << 7) | 0x37; }
constexpr uint32_t csrrs_(int rd, int csr, int rs1) { return i_type(0x73, rd, 2, rs1, csr); }
constexpr uint32_t csrrw_(int rd, int csr, int rs1) { return i_type(0x73, rd, 1, rs1, csr); }
constexpr uint32_t lw_(int rd, int rs1, int32_t imm) { return i_type(0x03, rd, 2, rs1, imm); }
constexpr uint32_t sw_(int rs1, int rs2, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((rs2 & 0x1F) << 20)
         | ((rs1 & 0x1F) << 15) | (2u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x23u;
}
constexpr uint32_t amo_(int rd, int rs1, int rs2, uint32_t funct5) {
    return (funct5 << 27) | ((rs2 & 0x1F) << 20) | ((rs1 & 0x1F) << 15)
         | (2u << 12) | ((rd & 0x1F) << 7) | 0x2Fu;
}
constexpr uint32_t spin_() { return 0x0000006Fu; }

} // namespace

// AMOMIN/AMOMAX 必须按有符号比较：INT32_MIN 和 1 比，signed min 是 INT32_MIN
TEST(Rv32Edge, AmoMinMaxUseSignedCompareAtInt32Min) {
    PrivTestFixture f;
    f.loadProgram({
        lui_(1, 0x1), addi_(1, 1, 0x100),          // x1 = 0x1100
        lui_(2, 0x80000),                          // x2 = 0x80000000（INT32_MIN）
        sw_(1, 2, 0),                              // sw x2, 0(x1)
        addi_(3, 0, 1),                            // x3 = 1
        amo_(4, 1, 3, 0x10),                       // amomin.w x4, x3, (x1)
        lw_(5, 1, 0),                              // lw x5, 0(x1)
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(200);
    EXPECT_EQ((uint32_t) f.cpu->reg(4), 0x80000000u) << "AMOMIN 的旧值不对";
    EXPECT_EQ((uint32_t) f.cpu->reg(5), 0x80000000u) << "AMOMIN 用了无符号比较";
}

// 未实现的 CSR 必须报非法指令，不能静默返回 0
TEST(Rv32Edge, UnimplementedCsrTraps) {
    PrivTestFixture f;
    f.loadProgram({
        csrrs_(1, 0x7C0, 0),   // 本核不实现的 CSR
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(40);
    EXPECT_EQ(f.cpu->pc(), 0u) << "非法 CSR 没有 trap";
    EXPECT_EQ((uint32_t) f.cpu->reg(1), 0u) << "非法 CSR 不该写回 rd";
}

// CSR 只读字段写不进去：misa 的 MXL 必须恒为 1（RV32）
TEST(Rv32Edge, MisaMxlStaysOne) {
    PrivTestFixture f;
    f.loadProgram({
        addi_(1, 0, -1),
        csrrw_(0, 0x301, 1),   // 往 misa 写全 1
        csrrs_(2, 0x301, 0),   // x2 = misa
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(60);
    EXPECT_EQ(((uint32_t) f.cpu->reg(2) >> 30) & 3u, 1u) << "misa 的 MXL 被写坏了";
}

// SC 成功路径：LR 之后没人动过保留地址，SC 返回 0 并写入
TEST(Rv32Edge, ScSucceedsWhenReservationIntact) {
    PrivTestFixture f;
    f.loadProgram({
        lui_(1, 0x1), addi_(1, 1, 0x100),
        addi_(2, 0, 7),
        sw_(1, 2, 0),                              // sw x2, 0(x1)
        amo_(4, 1, 0, 0x02),                       // lr.w x4, (x1)
        addi_(5, 0, 9),
        amo_(6, 1, 5, 0x03),                       // sc.w x6, x5, (x1)
        lw_(7, 1, 0),
        spin_(),
    });
    f.compile(); f.reset(); f.runCycles(240);
    EXPECT_EQ((uint32_t) f.cpu->reg(6), 0u) << "保留有效时 SC 却失败了";
    EXPECT_EQ((uint32_t) f.cpu->reg(7), 9u) << "SC 成功但内存没更新";
}
