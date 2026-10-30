// 浮点舍入与标志位的回归测试：这些点都是 DeepSeek 网页版审核指出的真实缺陷，
// 每条用例对应一个曾经写错的实现（非 RNE 模式对已舍入值二次舍入、sNaN 漏报 NV、
// FS 不置 Dirty、half 的 NaN-boxing 漏检）。
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "platform64.h"

namespace {

// OP-FP 编码：funct7 = op<<2 | fmt。op 是规范里 funct7[6:2] 的部分
constexpr uint32_t enc_fp(int rd, int f3, int rs1, int rs2, uint32_t op, int fmt) {
    return ((op << 2) | (uint32_t) fmt) << 25 | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(f3 & 7) << 12)
         | ((uint32_t)(rd & 0x1F) << 7) | 0x53u;
}
constexpr uint32_t OP_FADD = 0x00, OP_FSUB = 0x01, OP_FMUL = 0x02, OP_FDIV = 0x03;
constexpr uint32_t OP_FSQRT = 0x0B, OP_FSGNJ = 0x04, OP_FMIN = 0x05;
constexpr uint32_t OP_FMV_W_X = 0x1E, OP_FCVT_W_S = 0x18, OP_FCVT_S_W = 0x1A;
constexpr uint32_t ld(int rd, int rs1, int32_t imm) { return enc_i(0x03, rd, 3, rs1, imm); }

constexpr int RM_RNE = 0, RM_RTZ = 1, RM_RDN = 2, RM_RUP = 3, RM_RMM = 4;

// 数据区放在指令区之后，避免 load() 覆盖
constexpr uint32_t DATA = 0x200;

struct FpFixture : Rv64Fixture {
    void put64(uint32_t off, uint64_t v) {
        uint8_t *m = mem->writableMemPtr() + off;
        for (int i = 0; i < 8; i++) m[i] = (uint8_t)(v >> (8 * i));
    }
    // 把常量装进通用寄存器：LD 是全 64 位搬运，比一串 lui/addi 干净
    uint32_t loadImm(int rd, uint64_t v) {
        put64(DATA + slot * 8, v);
        return ld(rd, 0, (int32_t)(DATA + slot++ * 8));
    }
    uint32_t slot = 0;

    // 浮点指令在 mstatus.FS=Off 时会当作非法指令 trap，测试前先把 FS 置成 Initial
    void loadFp(std::vector<uint32_t> body) {
        std::vector<uint32_t> all = {loadImm(5, 1u << 13), csrrw(0, 0x300, 5)};
        all.insert(all.end(), body.begin(), body.end());
        load(all);
    }

    bool nx() const { return (cpu->fcsr() & 0x01) != 0; }
    bool nv() const { return (cpu->fcsr() & 0x10) != 0; }
};

} // namespace

// FCVT.D.L 超过 2^53 时必须置 NX：旧实现拿 (double)v1 和自己比，恒相等
TEST(FpRounding, FcvtDLIntSetsInexact) {
    FpFixture f;
    f.loadFp({f.loadImm(1, (1ULL << 53) + 1), enc_fp(1, RM_RNE, 1, 2, OP_FCVT_S_W, 1), spin()});
    f.run(40);
    EXPECT_EQ(f.cpu->freg64(1), 0x4340000000000000ULL) << "2^53+1 应舍入成 2^53";
    EXPECT_TRUE(f.nx()) << "整数转 double 丢精度必须置 NX";
}

// 2^53+3 是 2^53+2 与 2^53+4 的中点：RDN 要往下取，RNE 取偶往上
TEST(FpRounding, FcvtDLRespectsRdDown) {
    FpFixture f;
    f.loadFp({f.loadImm(1, (1ULL << 53) + 3), enc_fp(1, RM_RDN, 1, 2, OP_FCVT_S_W, 1), spin()});
    f.run(40);
    EXPECT_EQ(f.cpu->freg64(1), 0x4340000000000001ULL) << "RDN 应取 2^53+2";
    EXPECT_TRUE(f.nx());
}

// 1.0 + (-2^-54) 落在 1.0 与 1-2^-53 的正中：RNE 取偶得 1.0，RDN 必须退到 1-2^-53
TEST(FpRounding, FaddDRdDownMidpoint) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x3FF0000000000000ULL),   // 1.0
        f.loadImm(2, 0xBC90000000000000ULL),   // -2^-54
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),      // fmv.d.x f1, x1
        enc_fp(2, 0, 2, 0, OP_FMV_W_X, 1),
        enc_fp(3, RM_RDN, 1, 2, OP_FADD, 1),    // fadd.d f3, f1, f2, rdn
        spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->freg64(3), 0x3FEFFFFFFFFFFFFFULL) << "RDN 的中点必须向下";
    EXPECT_TRUE(f.nx());
}

// RTZ 对同一组输入也向下，RUP 则留在 1.0
TEST(FpRounding, FaddDRtzAndRupMidpoint) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x3FF0000000000000ULL),
        f.loadImm(2, 0xBC90000000000000ULL),
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(2, 0, 2, 0, OP_FMV_W_X, 1),
        enc_fp(3, RM_RTZ, 1, 2, OP_FADD, 1),
        enc_fp(4, RM_RUP, 1, 2, OP_FADD, 1),
        spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->freg64(3), 0x3FEFFFFFFFFFFFFFULL) << "RTZ 应截断到 1-2^-53";
    EXPECT_EQ(f.cpu->freg64(4), 0x3FF0000000000000ULL) << "RUP 应向上到 1.0";
}

// (1+2^-52)(1-2^-52) = 1-2^-104 略小于 1.0：RNE 给 1.0，RTZ 必须退一位
TEST(FpRounding, FmulDRtzTruncates) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x3FF0000000000001ULL),   // 1 + 2^-52
        f.loadImm(2, 0x3FEFFFFFFFFFFFFEULL),   // 1 - 2^-52
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(2, 0, 2, 0, OP_FMV_W_X, 1),
        enc_fp(3, RM_RTZ, 1, 2, OP_FMUL, 1),
        enc_fp(4, RM_RNE, 1, 2, OP_FMUL, 1),
        spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->freg64(4), 0x3FF0000000000000ULL) << "RNE 结果应为 1.0";
    EXPECT_EQ(f.cpu->freg64(3), 0x3FEFFFFFFFFFFFFFULL) << "RTZ 应截断成 1-2^-53";
    EXPECT_TRUE(f.nx());
}

// 1/3 的 RNE 结果略小于真值，RUP 要补一个 ulp
TEST(FpRounding, FdivDRupStepsUp) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x3FF0000000000000ULL),   // 1.0
        f.loadImm(2, 0x4008000000000000ULL),   // 3.0
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(2, 0, 2, 0, OP_FMV_W_X, 1),
        enc_fp(3, RM_RUP, 1, 2, OP_FDIV, 1),
        enc_fp(4, RM_RDN, 1, 2, OP_FDIV, 1),
        spin(),
    });
    f.run(60);
    const uint64_t rne = 0x3FD5555555555555ULL;   // 1/3 的最近值
    EXPECT_EQ(f.cpu->freg64(4), rne) << "RDN 与 RNE 相同（RNE 已偏小）";
    EXPECT_EQ(f.cpu->freg64(3), rne + 1) << "RUP 必须再进一个 ulp";
}

// sqrt(2) 的 RNE 结果与真值同侧关系：用参考实现算出该往哪边补
TEST(FpRounding, FsqrtDRtzMatchesTruncation) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x4000000000000000ULL),   // 2.0
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(3, RM_RTZ, 1, 0, OP_FSQRT, 1),
        enc_fp(4, RM_RUP, 1, 0, OP_FSQRT, 1),
        spin(),
    });
    f.run(60);
    const double exact = std::sqrt(2.0);
    // 用 fma 精确比较 exact² 与 2：exact 落在真值哪一侧决定 RTZ/RUP 该往哪边补
    const double r2 = std::fma(exact, exact, -2.0);
    const double want_rtz = (r2 > 0.0) ? std::nextafter(exact, 0.0) : exact;
    const double want_rup = (r2 < 0.0) ? std::nextafter(exact, 2.0) : exact;
    uint64_t w_rtz, w_rup;
    std::memcpy(&w_rtz, &want_rtz, 8);
    std::memcpy(&w_rup, &want_rup, 8);
    EXPECT_EQ(f.cpu->freg64(3), w_rtz);
    EXPECT_EQ(f.cpu->freg64(4), w_rup);
    EXPECT_TRUE(f.nx());
}

// 两个 sNaN 输入时 fmin 也要置 NV
TEST(FpRounding, FminBothSnanSetsNv) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x7FF0000000000001ULL),   // signaling NaN
        f.loadImm(2, 0x7FF0000000000002ULL),
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(2, 0, 2, 0, OP_FMV_W_X, 1),
        enc_fp(3, 0, 1, 2, OP_FMIN, 1),         // fmin.d f3, f1, f2
        spin(),
    });
    f.run(60);
    EXPECT_TRUE(f.nv()) << "任一 sNaN 输入都必须置 NV，哪怕两个都是 NaN";
    EXPECT_EQ(f.cpu->freg64(3), 0x7FF8000000000000ULL);
}

// half 的 FSGNJ 读到非 NaN-boxed 寄存器时，数值域要变成 canonical NaN
TEST(FpRounding, FsgnjHChecksNanBox) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x0000000000000000ULL),   // 高 48 位不是全 1：不是合法的 half
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),      // fmv.d.x f1, x1
        enc_fp(2, 0, 1, 1, OP_FSGNJ, 2),        // fsgnj.h f2, f1, f1
        spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->freg64(2), 0xFFFFFFFFFFFF7E00ULL)
        << "非 boxed 输入按规范整体当作 canonical NaN";
}

// 双精度写 freg 后必须把 mstatus.FS 置 Dirty，否则上下文切换不保存浮点状态
TEST(FpRounding, DoubleWriteSetsFsDirty) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0x3FF0000000000000ULL),
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(2, 0, 1, 1, OP_FADD, 1),
        spin(),
    });
    f.run(60);
    EXPECT_EQ((f.cpu->mstatus() >> 13) & 3u, 3u) << "FADD.D 之后 FS 必须是 Dirty";
}

// half 写 freg 同样要置 FS
TEST(FpRounding, HalfWriteSetsFsDirty) {
    FpFixture f;
    f.loadFp({
        f.loadImm(1, 0xFFFFFFFFFFFF3C00ULL),   // half 1.0，已 NaN-box
        enc_fp(1, 0, 1, 0, OP_FMV_W_X, 1),
        enc_fp(2, 0, 1, 1, OP_FADD, 2),        // fadd.h f2, f1, f1
        spin(),
    });
    f.run(60);
    EXPECT_EQ((f.cpu->mstatus() >> 13) & 3u, 3u) << "FADD.H 之后 FS 必须是 Dirty";
}
