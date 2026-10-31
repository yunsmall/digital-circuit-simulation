// RV64 测试共用的最小平台：CPU + WishboneMemory + 引脚连线，外加指令编码辅助。
// 与 RV32 那套完全分开，避免两个位宽的夹具互相干扰。
#pragma once
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <gtest/gtest.h>

using namespace dsc;

namespace rv64test {

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
} // namespace rv64test

using namespace rv64test;