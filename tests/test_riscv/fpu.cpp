// F 扩展（单精度浮点）：运算、转换、比较、访存
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>

#include <cstring>
#include <vector>

using namespace dsc;

namespace {

struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk = nullptr, *rst = nullptr, *irq = nullptr, *mti = nullptr, *msi = nullptr;
    Net *wb_adr = nullptr, *wb_dat_o = nullptr, *wb_dat_i = nullptr, *wb_we = nullptr;
    Net *wb_sel = nullptr, *wb_stb = nullptr, *wb_cyc = nullptr, *wb_ack = nullptr;
    Net *halted = nullptr;

    Fixture() {
        auto c = std::make_unique<RiscVCPU>("cpu0");
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
        circuit->setWire(clk->id(), {0});
        circuit->setWire(rst->id(), {1});
        circuit->tick();
        circuit->setWire(rst->id(), {0});
        for (int i = 0; i < cycles; ++i) {
            circuit->setWire(clk->id(), {0});
            circuit->tick();
            circuit->setWire(clk->id(), {1});
            circuit->tick();
        }
    }
};

// 常用指令编码
uint32_t enc_addi(int rd, int rs1, int imm) { return ((uint32_t)(imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }
uint32_t enc_lui(int rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x37; }
uint32_t enc_sw(int rs2, int rs1, int imm) {
    return (((uint32_t)(imm & 0xFE0) << 20)) | (rs2 << 20) | (rs1 << 15) | (2u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x23;
}
uint32_t enc_lw(int rd, int rs1, int imm) { return ((uint32_t)(imm & 0xFFF) << 20) | (rs1 << 15) | (2u << 12) | (rd << 7) | 0x03; }
uint32_t enc_flw(int rd, int rs1, int imm) { return ((uint32_t)(imm & 0xFFF) << 20) | (rs1 << 15) | (2u << 12) | (rd << 7) | 0x07; }
uint32_t enc_fsw(int rs2, int rs1, int imm) {
    return (((uint32_t)(imm & 0xFE0) << 20)) | (rs2 << 20) | (rs1 << 15) | (2u << 12) | ((uint32_t)(imm & 0x1F) << 7) | 0x27;
}
static uint32_t enc_fcvt(int rd, int rs1, int rm, int rs2) {
    return (0x68u << 25) | ((uint32_t) rs2 << 20) | ((uint32_t) rs1 << 15)
           | ((uint32_t) rm << 12) | ((uint32_t) rd << 7) | 0x53u;
}
static uint32_t enc_jal0() { return 0x0000006Fu; }

uint32_t enc_fp(uint32_t f7, int rd, int rs1, int rs2) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x53;
}

} // namespace

TEST(FpuTest, AddAndSub) {
    Fixture f;
    f.load({
            enc_lui(5, 0x10000),                 // x5 = 0x10000000（寄存器造数用 lui+addi）
            enc_addi(1, 0, 0x3F8),               // 用位模式直接造 1.0f 的高位不方便，改用 FMV
            enc_fp(0x78, 1, 1, 0),               // FMV.W.X f1, x1
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
            enc_lui(5, 0x0),       // x5 = 0
            enc_addi(5, 0, 0x200), // x5 = 0x200
            enc_flw(1, 5, 0),      // f1 = 1.5
            enc_flw(2, 5, 4),      // f2 = 2.5
            enc_fp(0x00, 3, 1, 2), // f3 = f1 + f2 = 4.0
            enc_fp(0x04, 4, 2, 1), // f4 = f2 - f1 = 1.0
            enc_fp(0x08, 6, 1, 2), // f6 = f1 * f2 = 3.75
            enc_fp(0x0C, 7, 2, 1), // f7 = f2 / f1 = 1.666..
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
            enc_addi(5, 0, 0x200),
            enc_flw(1, 5, 0),      // f1 = 3.14159
            enc_fp(0x60, 6, 1, 0), // FCVT.W.S x6, f1 = 3（RNE）
            enc_addi(7, 0, 10),
            enc_fp(0x68, 2, 7, 0), // FCVT.S.W f2, x7 = 10.0
            enc_fp(0x50, 8, 1, 2), // FLT: 3.14 < 10 -> 1
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
            enc_addi(5, 0, 0x200),
            enc_flw(1, 5, 0),
            enc_flw(2, 5, 4),
            enc_fp(0x0C, 3, 1, 2), // 1.0 / 0.0 = +inf
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
        enc_addi(1, 0, -1),          // x1 = 0xFFFFFFFF（按无符号是 4294967295）
        enc_fcvt(5, 1, 1, 1),        // fcvt.s.wu f5, x1, RTZ(1)  rs2=1 表示无符号
        enc_jal0(),
    });
    f.run(80);
    EXPECT_EQ(f.cpu->freg(5), 0x4F7FFFFFu) << "RTZ 应向下截断而不是就近舍入";
}

