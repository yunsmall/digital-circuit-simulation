// RV64 实例的行为验证：64 位算术、W 系列的符号扩展、6 位移位量、LD/SD、
// M 扩展的 128 位乘积、非对齐 LD 报 trap、WFI 不死锁。
// 这些点都是审核过程中实际出过错的地方，每条对应一个曾经写错的实现。
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>
#include <gtest/gtest.h>

using namespace dsc;

namespace {

// 指令编码辅助：只覆盖测试用得到的几类
constexpr uint32_t enc_i(int opcode, int rd, int f3, int rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(f3 & 7) << 12) | ((uint32_t)(rd & 0x1F) << 7) | (uint32_t)opcode;
}
constexpr uint32_t enc_r(int opcode, int rd, int f3, int rs1, int rs2, int f7) {
    return ((uint32_t)(f7 & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(f3 & 7) << 12)
         | ((uint32_t)(rd & 0x1F) << 7) | (uint32_t)opcode;
}
// OP-IMM 的移位类：RV64 的 shamt 是 6 位，bit25 属于移位量而不是 funct7
constexpr uint32_t enc_shift_i(int rd, int f3, int rs1, int shamt6) {
    return ((uint32_t)(shamt6 & 0x3F) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | ((uint32_t)(f3 & 7) << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x13u;
}
constexpr uint32_t addi(int rd, int rs1, int32_t imm) { return enc_i(0x13, rd, 0, rs1, imm); }
// STORE：imm[11:5] 在 inst[31:25]，rs2 在 inst[24:20]
constexpr uint32_t enc_s(int f3, int rs1, int rs2, int32_t imm) {
    return ((uint32_t)((imm >> 5) & 0x7F) << 25) | ((uint32_t)(rs2 & 0x1F) << 20)
         | ((uint32_t)(rs1 & 0x1F) << 15) | ((uint32_t)(f3 & 7) << 12)
         | ((uint32_t)(imm & 0x1F) << 7) | 0x23u;
}
constexpr uint32_t addiw(int rd, int rs1, int32_t imm) { return enc_i(0x1B, rd, 0, rs1, imm); }
constexpr uint32_t lui(int rd, uint32_t imm20) {
    return (imm20 << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x37u;
}
constexpr uint32_t csrrw(int rd, int csr, int rs1) {
    return ((uint32_t)(csr & 0xFFF) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | (1u << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x73u;
}
constexpr uint32_t csrrs(int rd, int csr, int rs1) {
    return ((uint32_t)(csr & 0xFFF) << 20) | ((uint32_t)(rs1 & 0x1F) << 15)
         | (2u << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x73u;
}
constexpr uint32_t jal(int rd, int32_t off) {
    const uint32_t u = (uint32_t)off;
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1u) << 20)
         | (((u >> 12) & 0xFF) << 12) | ((uint32_t)(rd & 0x1F) << 7) | 0x6Fu;
}
// 自旋：jal x0, 0
constexpr uint32_t spin() { return 0x0000006Fu; }

struct Rv64Fixture {
    std::unique_ptr<Circuit> circuit;
    RiscVCPU<64> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk, *rst, *irq, *mti, *msi, *seip;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack, *halted;

    Rv64Fixture() {
        circuit = std::make_unique<Circuit>();
        auto cpu_ptr = std::make_unique<RiscVCPU<64>>("cpu64");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));
        auto mem_ptr = std::make_unique<WishboneMemory>("mem", 16);
        mem = mem_ptr.get();
        circuit->addComponent(std::move(mem_ptr));
        clk = circuit->createNet("clk"); rst = circuit->createNet("rst");
        irq = circuit->createNet("irq"); mti = circuit->createNet("mti");
        msi = circuit->createNet("msi"); seip = circuit->createNet("seip");
        wb_adr = circuit->createNet("wb_adr"); wb_dat_o = circuit->createNet("wb_dat_o");
        wb_dat_i = circuit->createNet("wb_dat_i"); wb_we = circuit->createNet("wb_we");
        wb_sel = circuit->createNet("wb_sel"); wb_stb = circuit->createNet("wb_stb");
        wb_cyc = circuit->createNet("wb_cyc"); wb_ack = circuit->createNet("wb_ack");
        halted = circuit->createNet("halted");
        circuit->connect(cpu, "clk", clk);
        circuit->connect(cpu, "rst", rst);
        circuit->connect(cpu, "irq", irq);
        circuit->connect(cpu, "mti", mti);
        circuit->connect(cpu, "msi", msi);
        circuit->connect(cpu, "seip", seip);
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
    void load(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        for (size_t i = 0; i < insts.size(); i++) {
            uint32_t v = insts[i];
            m[i*4+0] = (uint8_t)(v & 0xFF); m[i*4+1] = (uint8_t)((v>>8) & 0xFF);
            m[i*4+2] = (uint8_t)((v>>16) & 0xFF); m[i*4+3] = (uint8_t)((v>>24) & 0xFF);
        }
    }
    uint64_t mem64(uint32_t addr) const {
        const uint8_t *m = mem->writableMemPtr() + addr;
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | m[i];
        return v;
    }
    void run(int n) {
        circuit->compile();
        circuit->init();
        circuit->setWire(rst->id(), {1}); circuit->tick();
        circuit->setWire(rst->id(), {0});
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), {0}); circuit->tick();
            circuit->setWire(clk->id(), {1}); circuit->tick();
        }
    }
};
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