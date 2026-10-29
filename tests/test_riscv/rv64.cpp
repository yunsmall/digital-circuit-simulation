// RV64 实例的最小验证：64 位算术、W 系列、LD/SD
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>
#include <gtest/gtest.h>

using namespace dsc;

namespace {
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

// 64 位加法必须真的按 64 位算，不能被截成 32 位
TEST(Rv64Test, AddWideValue) {
    Rv64Fixture f;
    f.load({
        0x00001337u, // 0x00: lui x6, 1        x6 = 0x1000
        0x0000031Bu, // 0x04: addiw x6, x6, 0  x6 = 0x1000
        0x00000013u, // 0x08: nop
        0x00000013u, // 0x0C: nop
        0x00000013u, // 0x10: nop
        0x00000013u, // 0x14: nop
        0x00000013u, // 0x18: nop
        0x00000013u, // 0x1C: nop
        0x0000006Fu, // 0x20: jal x0, 0
    });
    f.run(60);
    EXPECT_EQ((uint64_t) f.cpu->reg(6), 0x1000ULL);
}
