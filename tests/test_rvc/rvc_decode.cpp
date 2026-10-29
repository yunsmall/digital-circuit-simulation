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
