// F 扩展（单精度浮点）：运算、转换、比较、访存
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>

#include <cstring>
#include <vector>

#include "enc.h"

using namespace dsc;
using namespace rv32enc;

namespace {

struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk = nullptr, *rst = nullptr, *irq = nullptr, *mti = nullptr, *msi = nullptr;
    Net *wb_adr = nullptr, *wb_dat_o = nullptr, *wb_dat_i = nullptr, *wb_we = nullptr;
    Net *wb_sel = nullptr, *wb_stb = nullptr, *wb_cyc = nullptr, *wb_ack = nullptr;
    Net *halted = nullptr;

    Fixture() {
        auto c = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = c.get();
        circuit->addComponent(std::move(c));
        auto m = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = m.get();
        circuit->addComponent(std::move(m));
        clk = circuit->createNet("clk");
        rst = circuit->createNet("rst");
        irq = circuit->createNet("irq");
        mti = circuit->createNet("mti");
        msi = circuit->createNet("msi");
        wb_adr = circuit->createNet("wb_adr");
        wb_dat_o = circuit->createNet("wb_dat_o");
        wb_dat_i = circuit->createNet("wb_dat_i");
        wb_we = circuit->createNet("wb_we");
        wb_sel = circuit->createNet("wb_sel");
        wb_stb = circuit->createNet("wb_stb");
        wb_cyc = circuit->createNet("wb_cyc");
        wb_ack = circuit->createNet("wb_ack");
        halted = circuit->createNet("halted");
        circuit->connect(cpu, "clk", clk);
        circuit->connect(cpu, "rst", rst);
        circuit->connect(cpu, "irq", irq);
        circuit->connect(cpu, "mti", mti);
        circuit->connect(cpu, "msi", msi);
        circuit->connect(cpu, "wb_dat_i", wb_dat_i);
        circuit->connect(cpu, "wb_ack_i", wb_ack);
        circuit->connect(cpu, "wb_adr_o", wb_adr);
        circuit->connect(cpu, "wb_dat_o", wb_dat_o);
        circuit->connect(cpu, "wb_we_o", wb_we);
        circuit->connect(cpu, "wb_sel_o", wb_sel);
        circuit->connect(cpu, "wb_stb_o", wb_stb);
        circuit->connect(cpu, "wb_cyc_o", wb_cyc);
        circuit->connect(cpu, "halted", halted);
        circuit->connect(mem, "clk", clk);
        circuit->connect(mem, "wb_adr_i", wb_adr);
        circuit->connect(mem, "wb_dat_i", wb_dat_o);
        circuit->connect(mem, "wb_we_i", wb_we);
        circuit->connect(mem, "wb_sel_i", wb_sel);
        circuit->connect(mem, "wb_stb_i", wb_stb);
        circuit->connect(mem, "wb_cyc_i", wb_cyc);
        circuit->connect(mem, "wb_dat_o", wb_dat_i);
        circuit->connect(mem, "wb_ack_o", wb_ack);
    }

    void load(const std::vector<uint32_t> &insts, uint32_t at = 0) {
        // load 自动前置两条：lui x6,2（0x2000 = mstatus.FS=Initial）、csrrw x0,mstatus,x6。
        // 真实核 reset 后 FS 是 Off，此时任何浮点指令都会触发非法指令——
        // 裸机程序必须自己先把浮点单元打开
        std::vector<uint32_t> all;
        if (at == 0) {
            all.push_back(0x00002337u);
            all.push_back(0x30031073u);
        }
        all.insert(all.end(), insts.begin(), insts.end());

        uint8_t *m = mem->writableMemPtr();
        for (size_t i = 0; i < all.size(); i++) {
            uint32_t x = all[i], off = at + i * 4;
            m[off + 0] = (uint8_t)(x & 0xFF);
            m[off + 1] = (uint8_t)((x >> 8) & 0xFF);
            m[off + 2] = (uint8_t)((x >> 16) & 0xFF);
            m[off + 3] = (uint8_t)((x >> 24) & 0xFF);
        }
    }

    void run(int cycles = 60) {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), 0);
        circuit->setWire(rst->id(), 1);
        circuit->tick();
        circuit->setWire(rst->id(), 0);
        for (int i = 0; i < cycles; ++i) {
            circuit->setWire(clk->id(), 0);
            circuit->tick();
            circuit->setWire(clk->id(), 1);
            circuit->tick();
        }
    }
};

// 指令编码统一放在 enc.h

void put16(Fixture &f, uint32_t addr, uint16_t v) {
    std::memcpy(f.mem->writableMemPtr() + addr, &v, 2);
}
void put32(Fixture &f, uint32_t addr, uint32_t v) {
    std::memcpy(f.mem->writableMemPtr() + addr, &v, 4);
}
void put64(Fixture &f, uint32_t addr, uint64_t v) {
    std::memcpy(f.mem->writableMemPtr() + addr, &v, 8);
}

} // namespace

// IEEE 754：DZ 只针对"有限非零被除数 / 0"；双精度 Inf/0 的结果是 Inf，
// 不能置 DZ（单精度路径已经有 isfinite 判断）
TEST(FpuTest, DoubleDivInfByZeroDoesNotSetDz) {
    Fixture f;
    uint8_t *m = f.mem->writableMemPtr();
    const uint64_t inf = 0x7FF0000000000000ULL;
    const uint64_t zero = 0;
    std::memcpy(m + 0x200, &inf, 8);
    std::memcpy(m + 0x208, &zero, 8);
    std::memset(m + 0x210, 0, 8);

    f.load({
            addi(5, 0, 0x200),
            fld(1, 5, 0),         // f1 = +Inf
            fld(2, 5, 8),         // f2 = +0
            fp(0x0D, 3, 1, 2),    // fdiv.d f3, f1, f2（funct7 低位区分 D）
            fsd(3, 5, 16),        // 结果写回内存
            spin(),
    });
    f.run(80);

    EXPECT_EQ(f.cpu->freg64(3), inf) << "Inf/0 的结果应为 +Inf";
    EXPECT_EQ(f.cpu->fcsr() & 0x08u, 0u) << "Inf/0 不应置 DZ";
    uint64_t stored = 0;
    std::memcpy(&stored, m + 0x210, 8);
    EXPECT_EQ(stored, inf);
}

TEST(FpuTest, AddAndSub) {
    Fixture f;
    f.load({
            lui(5, 0x10000),                 // x5 = 0x10000000（寄存器造数用 lui+addi）
            addi(1, 0, 0x3F8),               // 用位模式直接造 1.0f 的高位不方便，改用 FMV
            fp(0x78, 1, 1, 0),               // FMV.W.X f1, x1
    });
    f.run(20);
    // 通路验证：f1 应等于 x1 的值
    EXPECT_EQ(f.cpu->freg(1), f.cpu->reg(1));
}

TEST(FpuTest, ArithmeticViaMemory) {
    Fixture f;
    // 在内存 0x200 放 1.5f 和 2.5f，用 FLW 读入后做 FADD/FSW
    f.load({
            0x100002B7u,            // 0x00: lui x5, 0x10000 -> x5 = 0x10000000？改用小地址
    });
    // 直接用更简单的方式：先把常量写进内存
    uint32_t d = 0x200;
    auto put = [&](uint32_t addr, uint32_t bits) {
        uint8_t *m = f.mem->writableMemPtr();
        std::memcpy(m + addr, &bits, 4);
    };
    put(0x200, 0x3FC00000u); // 1.5f
    put(0x204, 0x40200000u); // 2.5f
    f.load({
            lui(5, 0x0),       // x5 = 0
            addi(5, 0, 0x200), // x5 = 0x200
            flw(1, 5, 0),      // f1 = 1.5
            flw(2, 5, 4),      // f2 = 2.5
            fp(0x00, 3, 1, 2), // f3 = f1 + f2 = 4.0
            fp(0x04, 4, 2, 1), // f4 = f2 - f1 = 1.0
            fp(0x08, 6, 1, 2), // f6 = f1 * f2 = 3.75
            fp(0x0C, 7, 2, 1), // f7 = f2 / f1 = 1.666..
    }, 0);
    f.run(80);
    EXPECT_EQ(f.cpu->freg(3), 0x40800000u) << "1.5 + 2.5 = 4.0";
    EXPECT_EQ(f.cpu->freg(4), 0x3F800000u) << "2.5 - 1.5 = 1.0";
    EXPECT_EQ(f.cpu->freg(6), 0x40700000u) << "1.5 * 2.5 = 3.75";
}

TEST(FpuTest, ConvertAndCompare) {
    Fixture f;
    auto put = [&](uint32_t addr, uint32_t bits) {
        uint8_t *m = f.mem->writableMemPtr();
        std::memcpy(m + addr, &bits, 4);
    };
    put(0x200, 0x40490FDBu); // 3.14159f
    f.load({
            addi(5, 0, 0x200),
            flw(1, 5, 0),      // f1 = 3.14159
            fp(0x60, 6, 1, 0), // FCVT.W.S x6, f1 = 3（RNE）
            addi(7, 0, 10),
            fp(0x68, 2, 7, 0), // FCVT.S.W f2, x7 = 10.0
            fp(0x50, 8, 1, 2), // FLT: 3.14 < 10 -> 1
    });
    f.run(80);
    EXPECT_EQ(f.cpu->reg(6), 3u) << "FCVT.W.S 应截成 3";
    EXPECT_EQ(f.cpu->freg(2), 0x41200000u) << "10.0f 的位模式";
    EXPECT_EQ(f.cpu->reg(8), 1u) << "3.14 < 10 应为真";
}

TEST(FpuTest, DivideByZeroSetsFlag) {
    Fixture f;
    auto put = [&](uint32_t addr, uint32_t bits) {
        uint8_t *m = f.mem->writableMemPtr();
        std::memcpy(m + addr, &bits, 4);
    };
    put(0x200, 0x3F800000u); // 1.0f
    put(0x204, 0x00000000u); // 0.0f
    f.load({
            addi(5, 0, 0x200),
            flw(1, 5, 0),
            flw(2, 5, 4),
            fp(0x0C, 3, 1, 2), // 1.0 / 0.0 = +inf
    });
    f.run(80);
    EXPECT_EQ(f.cpu->freg(3), 0x7F800000u) << "除零应得到 +inf";
    EXPECT_NE(f.cpu->fcsr() & 0x08, 0u) << "应置 DZ 标志";
}

// 舍入模式必须真正生效：RTZ 与 RNE 对同一个不精确输入给出不同结果
// 0xFFFFFFFF（按无符号 4294967295）转 float：
//   RNE 得 2^32 = 4294967296（0x4F800000）
//   RTZ 得 4294967040（0x4F7FFFFF）
TEST(FpuTest, FcvtRespectsRoundingMode) {
    Fixture f;
    f.load({
        addi(1, 0, -1),          // x1 = 0xFFFFFFFF（按无符号是 4294967295）
        fcvt(5, 1, 1, 1),        // fcvt.s.wu f5, x1, RTZ(1)  rs2=1 表示无符号
        spin(),
    });
    f.run(80);
    EXPECT_EQ(f.cpu->freg(5), 0x4F7FFFFFu) << "RTZ 应向下截断而不是就近舍入";
}

// 规范 6.2.2：除 load/store/FMV 这类位传输外，使用窄值的浮点操作必须先检查
// NaN-boxing；高 32 位不是全 1 时输入按 canonical NaN 处理
TEST(FpuTest, FclassSChecksNanBoxing) {
    Fixture f;
    f.load({
        lui(5, 0x3F800),      // x5 = 0x3F800000（1.0f 的位模式）
        fp(0x79, 10, 5, 0),   // fmv.d.x f10, x5：高 32 位为 0，不是合法 box
        fp_f3(0x70, 1, 10, 10), // fclass.s a0, f10
    });
    f.run(40);
    EXPECT_EQ(f.cpu->reg(10), 1u << 9) << "非 NaN-boxed 的窄值应按 canonical NaN 分类";

    // 对照：FMV.W.X 写入的是合法 NaN-boxed 值，1.0f 应分类为正规格化
    f.load({
        lui(5, 0x3F800),
        fp(0x78, 10, 5, 0),     // fmv.w.x f10, x5
        fp_f3(0x70, 1, 10, 10), // fclass.s a0, f10
    });
    f.run(40);
    EXPECT_EQ(f.cpu->reg(10), 1u << 6) << "boxed 的 1.0f 是正规格化";
}

// F 扩展 6.1.6：FMAX(-0,-0) = -0，FMIN(+0,-0) = -0；结果必须保持 NaN-box。
// 曾经整体写 0 把高 32 位清零，后续单精度指令会把合法结果当成非 boxed 的 NaN
TEST(FpuTest, FminMaxZeroSignAndBoxing) {
    Fixture f;
    put32(f, 0x200, 0x00000000u);   // +0.0f
    put32(f, 0x204, 0x80000000u);   // -0.0f
    f.load({
            addi(5, 0, 0x200),
            flw(1, 5, 0),
            flw(2, 5, 4),
            fp_r(0x14, 1, 3, 2, 2),   // fmax.s f3, f2, f2（-0,-0）
            fp_r(0x14, 1, 4, 1, 2),   // fmax.s f4, f1, f2（+0,-0）
            fp_r(0x14, 0, 6, 1, 2),   // fmin.s f6, f1, f2（+0,-0）
            spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->freg64(3), 0xFFFFFFFF80000000ULL) << "FMAX(-0,-0) 应为 -0";
    EXPECT_EQ(f.cpu->freg64(4), 0xFFFFFFFF00000000ULL) << "FMAX(+0,-0) 应为 boxed +0";
    EXPECT_EQ(f.cpu->freg64(6), 0xFFFFFFFF80000000ULL) << "FMIN(+0,-0) 应为 -0";
}

// FCVT.S.D：±Inf 是精确转换，不能走溢出分支置 OF/NX；下溢的次正规结果要置 UF
TEST(FpuTest, FcvtSdInfIsExactAndUnderflowSetsUf) {
    Fixture f;
    put64(f, 0x200, 0x7FF0000000000000ULL);   // +Inf
    put64(f, 0x208, 0x37A16C262777579CULL);   // 1e-40（单精度次正规区间，不精确）
    f.load({
            addi(5, 0, 0x200),
            fld(1, 5, 0),
            fcvt_fp(0, 1, 2, 1),    // fcvt.s.d f2, f1
            fld(3, 5, 8),
            fcvt_fp(0, 1, 4, 3),    // fcvt.s.d f4, f3
            spin(),
    });
    f.run(80);
    EXPECT_EQ(f.cpu->freg64(2), 0xFFFFFFFF7F800000ULL) << "Inf 转换必须精确且保留 NaN-box";
    EXPECT_EQ(f.cpu->fcsr() & 0x1Fu, 0x03u) << "应为 UF|NX，不能有 OF";
}

// Zfh 的 FDIV.H 除零也要置 DZ（与单/双精度一致）
TEST(FpuTest, HalfDivByZeroSetsDz) {
    Fixture f;
    put16(f, 0x200, 0x3C00);   // 1.0h
    put16(f, 0x202, 0x0000);   // 0.0h
    f.load({
            addi(5, 0, 0x200),
            flh(1, 5, 0),
            flh(2, 5, 2),
            fp(0x0E, 3, 1, 2),   // fdiv.h f3, f1, f2
            spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->freg64(3) & 0xFFFFu, 0x7C00u) << "1/0 应得到 half 的 +Inf";
    EXPECT_NE(f.cpu->fcsr() & 0x08u, 0u) << "half 除法除零必须置 DZ";
}

// 舍入到 half 的次正规数且不精确时必须置 UF|NX
TEST(FpuTest, HalfUnderflowSetsUf) {
    Fixture f;
    put32(f, 0x200, 0x3727C5ACu);   // 1e-5f
    f.load({
            addi(5, 0, 0x200),
            flw(1, 5, 0),
            fcvt_fp(2, 0, 2, 1),   // fcvt.h.s f2, f1
            spin(),
    });
    f.run(60);
    EXPECT_EQ(f.cpu->fcsr() & 0x03u, 0x03u) << "下溢且不精确应为 UF|NX";
}

// 精确抵消成零时只有 RDN 给 -0
TEST(FpuTest, HalfAndDoubleCancelRdnGivesNegativeZero) {
    Fixture f;
    put16(f, 0x200, 0x3C00);                  // 1.0h
    put64(f, 0x208, 0x3FF0000000000000ULL);   // 1.0
    f.load({
            addi(5, 0, 0x200),
            flh(1, 5, 0),
            fp_r(0x06, 2, 2, 1, 1),       // fsub.h f2, f1, f1, RDN
            fld(3, 5, 8),
            fp_r(0x05, 2, 4, 3, 3),       // fsub.d f4, f3, f3, RDN
            spin(),
    });
    f.run(80);
    EXPECT_EQ(f.cpu->freg64(2), 0xFFFFFFFFFFFF8000ULL) << "fsub.h 精确抵消在 RDN 下应为 -0";
    EXPECT_EQ(f.cpu->freg64(4), 0x8000000000000000ULL) << "fsub.d 精确抵消在 RDN 下应为 -0";
}

// D 扩展：FMA 必须按 rm 舍入、溢出时按 rm 给最大有限值、下溢到零要置 UF|NX
TEST(FpuTest, DoubleFmaRoundingOverflowAndUnderflow) {
    Fixture f;
    put64(f, 0x200, 0x3FF0000000000001ULL);   // 1 + 2^-52
    put64(f, 0x208, 0x3FF0000000000000ULL);   // 1.0
    put64(f, 0x210, 0x3CA0000000000000ULL);   // 2^-53
    put64(f, 0x218, 0x7FEFFFFFFFFFFFFFULL);   // DBL_MAX
    put64(f, 0x220, 0x4000000000000000ULL);   // 2.0
    put64(f, 0x228, 0x1A70000000000000ULL);   // 2^-600
    put64(f, 0x230, 0x20B0000000000000ULL);   // 2^-500
    f.load({
            addi(5, 0, 0x200),
            fld(1, 5, 0),
            fld(2, 5, 8),
            fld(3, 5, 0x10),
            fma_d(0x43, 4, 1, 2, 3, 1),   // fmadd.d f4, RTZ：RNE 会进位，RTZ 不进
            fld(5, 5, 0x18),
            fld(6, 5, 0x20),
            fp_r(0x09, 1, 7, 5, 6),       // fmul.d f7, f5, f6, RTZ（DBL_MAX*2）
            fld(8, 5, 0x28),
            fld(9, 5, 0x30),
            fma_d(0x43, 10, 8, 9, 0, 0),  // fmadd.d f10 = 2^-1100（下溢到 0）
            spin(),
    });
    f.run(120);
    EXPECT_EQ(f.cpu->freg64(4), 0x3FF0000000000001ULL) << "FMA 没按 RTZ 舍入";
    EXPECT_EQ(f.cpu->freg64(7), 0x7FEFFFFFFFFFFFFFULL) << "RTZ 溢出应退回 DBL_MAX";
    EXPECT_EQ(f.cpu->freg64(10), 0ULL) << "2^-1100 应舍入到 0";
    EXPECT_EQ(f.cpu->fcsr() & 0x03u, 0x03u) << "下溢到零应置 UF|NX";
}

// 规范 6.1.7：Invalid 时不再置 NX；旧的 NV 标志也不能掩盖本次的 NX
TEST(FpuTest, FcvtIntegerNxRules) {
    Fixture f;
    put32(f, 0x200, 0xBF000000u);             // -0.5f
    put64(f, 0x208, 0x3FF8000000000000ULL);   // 1.5
    f.load({
            addi(5, 0, 0x200),
            flw(1, 5, 0),
            fcvt_w(6, 1, 2, 1),       // fcvt.wu.s x6, f1, RDN：-1 越界
            spin(),
    });
    f.run(60);
    EXPECT_NE(f.cpu->fcsr() & 0x10u, 0u) << "负值转无符号应置 NV";
    EXPECT_EQ(f.cpu->fcsr() & 0x01u, 0u) << "Invalid 置位时不应再置 NX";

    Fixture g;
    put64(g, 0x200, 0x3FF8000000000000ULL);   // 1.5
    g.load({
            addi(1, 0, 0x10),
            0x00109073u,                       // csrrw x0, fflags, x1：先置旧 NV
            addi(5, 0, 0x200),
            fld(1, 5, 0),
            fcvt_w(8, 1, 0, 0, 1),        // fcvt.w.d x8, f1：1.5 → 2
            spin(),
    });
    g.run(60);
    EXPECT_NE(g.cpu->fcsr() & 0x01u, 0u) << "旧的 NV 不应掩盖本次的 NX";
    EXPECT_EQ(g.cpu->reg(8), 2u);
}

// signaling NaN 参与跨精度转换必须置 NV；half 的 sNaN 在 halfToDouble 里
// 已经被统一成 quiet NaN，只能在源位模式上判
TEST(FpuTest, HalfSnanConversionSetsNv) {
    Fixture f;
    f.load({
            // addi 的立即数只有 12 位且会符号扩展，0x7C01 得用 lui+addi 拼
            lui(1, 0x8),             // x1 = 0x8000
            addi(1, 1, -0x3FF),      // x1 = 0x7C01
            fp(0x7A, 1, 1, 0),       // fmv.h.x f1, x1（boxed half sNaN）
            fcvt_fp(0, 2, 2, 1),     // fcvt.s.h f2, f1
            spin(),
    });
    f.run(40);
    EXPECT_EQ(f.cpu->freg(2), 0x7FC00000u) << "NaN 结果应规范化";
    EXPECT_NE(f.cpu->fcsr() & 0x10u, 0u) << "signaling NaN 转换必须置 NV";
}
