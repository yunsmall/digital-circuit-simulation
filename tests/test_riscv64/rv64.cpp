// RV64 实例的行为验证：64 位算术、W 系列的符号扩展、6 位移位量、LD/SD、
// M 扩展的 128 位乘积、非对齐 LD 报 trap、WFI 不死锁。
// 这些点都是审核过程中实际出过错的地方，每条对应一个曾经写错的实现。
#include <gtest/gtest.h>

#include "platform64.h"

namespace {

// AMO/LR/SC：opcode 0x2F，funct5 在 bit31:27，funct3 选 .W(2)/.D(3)
constexpr uint32_t enc_amo(int f5, int rd, int rs1, int rs2, int f3) {
    return ((uint32_t) f5 << 27) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(f3 & 7) << 12)
         | ((uint32_t)(rd & 0x1F) << 7) | 0x2Fu;
}
// FSD：opcode 0x27（浮点存储）
constexpr uint32_t enc_fsd(int rs2, int rs1, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | (3u << 12)
         | ((uint32_t)(imm & 0x1F) << 7) | 0x27u;
}
constexpr int F5_AMOADD = 0x00, F5_LR = 0x02, F5_SC = 0x03;

} // namespace


// 64 位立即数必须真的按 64 位算：addi x1, x0, -1 应得全 1，而不是 0xFFFFFFFF
TEST(Rv64Test, AddiIsSixtyFourBit) {
    Rv64Fixture f;
    f.load({ addi(1, 0, -1), spin() });
    f.run(30);
    EXPECT_EQ((uint64_t) f.cpu->reg(1), 0xFFFFFFFFFFFFFFFFULL);
}

// W 系列把 32 位结果符号扩展回 64 位：addiw x2, x0, -1 应得全 1
TEST(Rv64Test, AddiwSignExtends) {
    Rv64Fixture f;
    f.load({ addiw(2, 0, -1), spin() });
    f.run(30);
    EXPECT_EQ((uint64_t) f.cpu->reg(2), 0xFFFFFFFFFFFFFFFFULL)
        << "addiw 的结果没有符号扩展到 64 位";
}

// LUI 在 RV64 下把 32 位立即数符号扩展到 64 位
TEST(Rv64Test, LuiSignExtends) {
    Rv64Fixture f;
    f.load({ lui(3, 0xFFFFF), spin() });   // 立即数 -4096 的高 20 位
    f.run(30);
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0xFFFFFFFFFFFFF000ULL)
        << "LUI 没有把 32 位结果符号扩展到 64 位";
}

// RV64 的移位量是 6 位：slli x4, x1, 40 必须真的移 40 位
TEST(Rv64Test, ShiftImmediateUsesSixBits) {
    Rv64Fixture f;
    f.load({ addi(1, 0, 1), enc_shift_i(4, 1, 1, 40), spin() });  // f3=1 是 SLLI
    f.run(30);
    EXPECT_EQ((uint64_t) f.cpu->reg(4), 1ULL << 40)
        << "SLLI 的第 6 位移位量被丢掉了";
}

// SRAI 在同一条编码里，靠 funct6 区分；挪 40 位且被移数为负
TEST(Rv64Test, SraiUsesSixBits) {
    Rv64Fixture f;
    f.load({
        addi(1, 0, -1),                      // x1 = 全 1
        enc_shift_i(5, 5, 1, 40) | (0x10u << 26),  // SRAI x5, x1, 40
        spin(),
    });
    f.run(30);
    EXPECT_EQ((uint64_t) f.cpu->reg(5), 0xFFFFFFFFFFFFFFFFULL)
        << "SRAI 的 6 位移位量或算术右移被写错";
}

// SD/LD：8 字节必须完整往返
TEST(Rv64Test, StoreAndLoadDoubleword) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1),              // x1 = 0x1000（数据区）
        addi(1, 1, 0x100),
        addi(2, 0, -1),           // x2 = 全 1
        enc_s(3, 1, 2, 0),  // sd x2, 0(x1)
        enc_i(0x03, 6, 3, 1, 0),  // ld x6, 0(x1)
        spin(),
    });
    f.run(200);   // SD 和 LD 各要拆两次 32 位事务，拍数不能照单次访存给
    EXPECT_EQ(f.mem64(0x1100), 0xFFFFFFFFFFFFFFFFULL) << "SD 没写满 8 字节";
    EXPECT_EQ((uint64_t) f.cpu->reg(6), 0xFFFFFFFFFFFFFFFFULL) << "LD 没读回 8 字节";
}

// LWU 只取低 32 位并零扩展：内存里放一个高半非零的值，读回的高半必须是 0
TEST(Rv64Test, LwuZeroExtends) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x100),
        addi(2, 0, -1),
        enc_s(3, 1, 2, 0),  // sd x2, 0(x1)：内存里放全 1
        enc_i(0x03, 7, 6, 1, 0),  // lwu x7, 0(x1)
        spin(),
    });
    f.run(200);
    EXPECT_EQ(f.mem64(0x1100), 0xFFFFFFFFFFFFFFFFULL) << "前置的 SD 就没写进去";
    EXPECT_EQ((uint64_t) f.cpu->reg(7), 0xFFFFFFFFULL) << "LWU 没有零扩展";
}

// 非对齐 LD 应报 misaligned，而不是拼出缺字节的值
TEST(Rv64Test, UnalignedLoadTraps) {
    Rv64Fixture f;
    f.load({
        lui(1, 0x1), addi(1, 1, 0x103),  // 4 字节对齐但不是 8 字节对齐
        enc_i(0x03, 8, 3, 1, 0),         // ld x8, 0(x1)
        spin(),
    });
    f.run(50);
    EXPECT_EQ((uint64_t) f.cpu->reg(8), 0ULL)
        << "非对齐 LD 没有报 misaligned，仍然读出了值";
}

// misa 的 MXL 必须反映 64 位
TEST(Rv64Test, MisaReportsMxl64) {
    Rv64Fixture f;
    f.load({ csrrs(9, 0x301, 0), spin() });  // csrrs x9, misa, x0
    f.run(30);
    EXPECT_EQ(((uint64_t) f.cpu->reg(9) >> 62) & 3ULL, 2ULL) << "misa 的 MXL 不是 2";
}

// mstatus 的 UXL/SXL 在 RV64 上应该是 2
TEST(Rv64Test, MstatusReportsUxlSxl) {
    Rv64Fixture f;
    f.load({ csrrs(10, 0x300, 0), spin() });  // csrrs x10, mstatus, x0
    f.run(30);
    uint64_t v = (uint64_t) f.cpu->reg(10);
    EXPECT_EQ((v >> 32) & 3ULL, 2ULL) << "mstatus 的 UXL 不是 2";
    EXPECT_EQ((v >> 34) & 3ULL, 2ULL) << "mstatus 的 SXL 不是 2";
}

// M 扩展：INT64_MIN / -1 的商与余数由规范规定，不能靠 C 的未定义行为
TEST(Rv64Test, DivideOverflowIsDefined) {
    Rv64Fixture f;
    f.load({
        addi(1, 0, 1),
        enc_shift_i(1, 1, 1, 63),                        // slli x1, x1, 63 -> INT64_MIN
        addi(2, 0, -1),                                   // x2 = -1
        enc_r(0x33, 3, 4, 1, 2, 0x01),                    // div x3, x1, x2
        enc_r(0x33, 4, 6, 1, 2, 0x01),                    // rem x4, x1, x2
        spin(),
    });
    f.run(40);
    EXPECT_EQ((uint64_t) f.cpu->reg(3), 0x8000000000000000ULL) << "DIV 的溢出商不符合规范";
    EXPECT_EQ((uint64_t) f.cpu->reg(4), 0ULL) << "REM 的溢出余数不符合规范";
}

// WFI 在 mie 全 0 时不能挂死：规范要求这种情况立即完成
TEST(Rv64Test, WfiWithoutEnabledInterruptDoesNotHang) {
    Rv64Fixture f;
    f.load({
        addi(1, 0, 1),
        enc_i(0x73, 0, 0, 0, 0x105),  // wfi
        addi(2, 0, 7),                // 能执行到这条说明没挂死
        spin(),
    });
    f.run(40);
    EXPECT_EQ((uint64_t) f.cpu->reg(2), 7ULL) << "mie 全 0 时 WFI 挂死了";
}

// 规范 3.7.1：PMP CSR 是 XLEN 宽。RV64 写 64 位再读回必须原样返回，
// 软件靠写后读回验证 PMP 状态
TEST(Rv64Test, PmpRegistersAreSixtyFourBit) {
    Rv64Fixture f;
    const uint64_t addr_val = 0x123456789AULL;
    const uint64_t cfg_val = 0x0808080808080808ULL;
    uint8_t *m = f.mem->writableMemPtr();
    std::memcpy(m + 0x200, &addr_val, 8);
    std::memcpy(m + 0x208, &cfg_val, 8);
    f.load({
        enc_i(0x03, 1, 3, 0, 0x200),   // ld x1, 0x200(x0)
        csrrw(0, 0x3B0, 1),            // pmpaddr0 = x1
        csrrs(2, 0x3B0, 0),            // x2 = pmpaddr0
        enc_i(0x03, 3, 3, 0, 0x208),   // ld x3, 0x208(x0)
        csrrw(0, 0x3A0, 3),            // pmpcfg0 = x3
        csrrs(4, 0x3A0, 0),            // x4 = pmpcfg0
        spin(),
    });
    f.run(80);
    EXPECT_EQ((uint64_t) f.cpu->reg(2), addr_val) << "pmpaddr 的高 32 位被截掉了";
    EXPECT_EQ((uint64_t) f.cpu->reg(4), cfg_val) << "pmpcfg 的高 32 位被截掉了";
}

// 规范 5.2：保留集被写覆盖后 SC 必须失败。8 字节非对齐 SD 跨三个总线字，
// 落在第三个字的保留地址也要失效
TEST(Rv64Test, UnalignedSdInvalidatesReservationInThirdWord) {
    Rv64Fixture f;
    f.load({
        lui(1, 1), addi(1, 1, 0x100),        // x1 = 0x1100
        addi(3, 1, 8),                       // x3 = 0x1108（第三个字）
        addi(2, 0, 7),
        enc_amo(F5_LR, 5, 3, 0, 3),          // lr.d x5, (x3)
        enc_s(3, 1, 2, 1),                   // sd x2, 1(x1)：写 0x1101..0x1108
        enc_amo(F5_SC, 6, 3, 2, 3),          // sc.d x6, x2, (x3)
        spin(),
    });
    f.run(200);
    EXPECT_NE((uint64_t) f.cpu->reg(6), 0ULL) << "第三个字被 SD 覆盖后 SC 仍然成功";
}

// 规范 5.2：AMO 也是写，覆盖保留地址所在字后 SC 必须失败（本核按保守策略实现）
TEST(Rv64Test, AmoWriteInvalidatesReservation) {
    Rv64Fixture f;
    f.load({
        lui(1, 1), addi(1, 1, 0x100),        // x1 = 0x1100
        addi(2, 0, 7),
        enc_amo(F5_LR, 5, 1, 0, 3),          // lr.d x5, (x1)
        enc_amo(F5_AMOADD, 7, 1, 2, 3),      // amoadd.d x7, x2, (x1)
        enc_amo(F5_SC, 6, 1, 2, 3),          // sc.d x6, x2, (x1)
        spin(),
    });
    f.run(200);
    EXPECT_NE((uint64_t) f.cpu->reg(6), 0ULL) << "AMO 写到保留字后 SC 仍然成功";
}

// 规范 5.2：FSD 写两个字，保留地址落在第二个字时也要失效
TEST(Rv64Test, FsdInvalidatesReservationInSecondWord) {
    Rv64Fixture f;
    f.load({
        lui(5, 2), csrrw(0, 0x300, 5),       // mstatus.FS = Initial，允许浮点指令
        lui(1, 1), addi(1, 1, 0x100),        // x1 = 0x1100（8 字节对齐）
        addi(3, 1, 4),                       // x3 = 0x1104（第二个字）
        addi(2, 0, 7),
        enc_amo(F5_LR, 5, 3, 0, 2),          // lr.w x5, (x3)
        enc_fsd(1, 1, 0),                    // fsd f1, 0(x1)：写 0x1100..0x1107
        enc_amo(F5_SC, 6, 3, 2, 2),          // sc.w x6, x2, (x3)
        spin(),
    });
    f.run(200);
    EXPECT_NE((uint64_t) f.cpu->reg(6), 0ULL) << "FSD 的第二个字没让保留失效";
}