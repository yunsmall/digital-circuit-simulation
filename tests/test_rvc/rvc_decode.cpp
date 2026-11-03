// C 扩展解压表的单元测试：直接用已知的 16 位编码对照反汇编结果。
// 这类错误（立即数位段、寄存器域）在端到端测试里只会表现为"跑飞了"，
// 单测能直接指出是哪一条编码错了。
#include <gtest/gtest.h>
#include <dcs/util/RvcDecode.h>

using ::rvc::expand;
using ::rvc::ILLEGAL;

namespace {
// 从 32 位指令里取出各字段，方便断言
uint32_t rdOf(uint32_t i) { return (i >> 7) & 0x1F; }
uint32_t rs1Of(uint32_t i) { return (i >> 15) & 0x1F; }
uint32_t opOf(uint32_t i) { return i & 0x7F; }
int32_t immI(uint32_t i) { return (int32_t)i >> 20; }
} // namespace

// C.LI 0x4381 = li t2, 0 -> addi x7, x0, 0
TEST(RvcDecode, CLi) {
    const uint32_t e = expand<32>(0x4381);
    EXPECT_EQ(opOf(e), 0x13u);
    EXPECT_EQ(rdOf(e), 7u) << "C.LI 的 rd 解错";
    EXPECT_EQ(rs1Of(e), 0u);
    EXPECT_EQ(immI(e), 0) << "C.LI 的立即数解错";
}

// C.LI 0x4501 = li a0, 0 -> addi x10, x0, 0
TEST(RvcDecode, CLiA0) {
    const uint32_t e = expand<32>(0x4501);
    EXPECT_EQ(rdOf(e), 10u) << "C.LI 的 rd 解错";
}

// C.J 0xa0a1 = j 0x48 -> jal x0, 0x48
TEST(RvcDecode, CJ) {
    const uint32_t e = expand<32>(0xA0A1);
    EXPECT_EQ(opOf(e), 0x6Fu) << "C.J 应解成 jal";
    EXPECT_EQ(rdOf(e), 0u);
}

// C.NOP 0x0001 -> addi x0, x0, 0
TEST(RvcDecode, CNop) {
    const uint32_t e = expand<32>(0x0001);
    EXPECT_EQ(opOf(e), 0x13u);
    EXPECT_EQ(rdOf(e), 0u);
    EXPECT_EQ(immI(e), 0);
}

// C.ADDI a0, -16 的立即数是 6 位有符号数，必须解成 -16 而不是 48
TEST(RvcDecode, CAddiNegativeImm) {
    // c.addi a0, -16：op=1, f3=0, rd=10, imm[5]=1, imm[4:0]=0b10000
    // 编码 0x1541 = c.addi a0, -16（imm 的 6 位补码是 0b110000）
    const uint16_t enc = 0x1541;
    const uint32_t e = expand<32>(enc);
    EXPECT_EQ(opOf(e), 0x13u);
    EXPECT_EQ(rdOf(e), 10u);
    EXPECT_EQ(immI(e), -16) << "C.ADDI 的负立即数没符号扩展";
}

// C.FLD/C.FSD/C.FLDSP/C.FSDSP 属于 D 扩展的压缩形式。以前这里直接返回 ILLEGAL，
// clang 用 rv64gc 编译出的代码会因此触发非法指令 trap
TEST(RvcDecode, CFld) {
    // f3=001, rs1'=x9, rd'=x10, uimm=0
    const uint32_t e = expand<64>(0x2088);
    EXPECT_EQ(opOf(e), 0x07u) << "C.FLD 应解成 fld";
    EXPECT_EQ((e >> 12) & 7u, 3u) << "fld 的 funct3 必须是 3（双精度）";
    EXPECT_EQ(rdOf(e), 10u);
    EXPECT_EQ((e >> 15) & 0x1Fu, 9u) << "基址寄存器是 rs1'";
}

TEST(RvcDecode, CFsd) {
    // f3=101, rs1'=x9, rs2'=x10, uimm=0
    const uint32_t e = expand<64>(0xA088);
    EXPECT_EQ(opOf(e), 0x27u) << "C.FSD 应解成 fsd";
    EXPECT_EQ((e >> 12) & 7u, 3u);
}

TEST(RvcDecode, CFldsp) {
    // f3=001, rd=x10, uimm=0
    const uint32_t e = expand<64>(0x2502);
    EXPECT_EQ(opOf(e), 0x07u) << "C.FLDSP 应解成 fld";
    EXPECT_EQ(rdOf(e), 10u);
    EXPECT_EQ((e >> 15) & 0x1Fu, 2u) << "基址固定是 sp";
}

TEST(RvcDecode, CFsdsp) {
    // f3=101, rs2=x10, uimm=0
    const uint32_t e = expand<64>(0xA02A);
    EXPECT_EQ(opOf(e), 0x27u) << "C.FSDSP 应解成 fsd";
    EXPECT_EQ((e >> 15) & 0x1Fu, 2u);
}

// RV32 的 C.FSW 是浮点存储，opcode 必须是 0x27；以前误用了整数的 S()
TEST(RvcDecode, CFswRv32IsFloatStore) {
    // f3=111, rs1'=x8, rs2'=x8, uimm=0 -> c.fsw fs0, 0(s0)
    const uint32_t e = expand<32>(0xE000);
    EXPECT_EQ(opOf(e), 0x27u) << "C.FSW 应解成 fsw（opcode 0x27）";
    EXPECT_EQ((e >> 12) & 7u, 2u) << "fsw 的 funct3 必须是 2（单精度）";
    EXPECT_EQ((e >> 20) & 0x1Fu, 8u) << "rs2' 在 rs2 字段";
    EXPECT_EQ(rs1Of(e), 8u);
    // RV64 同一编码是 C.SD，仍应是整数存储
    EXPECT_EQ(opOf(expand<64>(0xE000)), 0x23u) << "RV64 的 C.SD 是整数存储";
}

TEST(RvcDecode, CFswspRv32IsFloatStore) {
    // f3=111, rs2=x8 -> c.fswsp fs0, 0(sp)
    const uint32_t e = expand<32>(0xE022);
    EXPECT_EQ(opOf(e), 0x27u) << "C.FSWSP 应解成 fsw（opcode 0x27）";
    EXPECT_EQ((e >> 12) & 7u, 2u);
    EXPECT_EQ(rs1Of(e), 2u) << "基址固定是 sp";
    EXPECT_EQ((e >> 20) & 0x1Fu, 8u) << "rs2 在 rs2 字段";
    EXPECT_EQ(opOf(expand<64>(0xE022)), 0x23u) << "RV64 的 C.SDSP 是整数存储";
}

// C.LUI rd=x0 是 HINT（合法空操作），不能报非法；规范明确 rd=x0 的码点按 HINT 处理
TEST(RvcDecode, CLuiZeroRdIsHint) {
    // f3=011, rd=0, imm=1 -> c.lui x0, 1
    const uint32_t e = expand<64>(0x6005);
    EXPECT_EQ(opOf(e), 0x37u) << "C.LUI 的 HINT 码点应按 lui 展开";
    EXPECT_EQ(rdOf(e), 0u);
}

// RV64 的 C.ADDIW rd=x0 是保留编码；同一编码在 RV32 是 C.JAL，仍然合法
TEST(RvcDecode, CAddiwZeroRdIsIllegal) {
    // f3=001, rd=0, imm=1 -> c.addiw x0, 1（保留）
    EXPECT_EQ(expand<64>(0x2005), ILLEGAL);
    EXPECT_EQ(opOf(expand<32>(0x2005)), 0x6Fu) << "RV32 同一编码是 C.JAL";
}
