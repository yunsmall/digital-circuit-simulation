// WFI：没有中断时停机并拉高 halted，中断到来后恢复执行
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk = nullptr, *rst = nullptr, *irq = nullptr, *mti = nullptr, *msi = nullptr;
    Net *wb_adr = nullptr, *wb_dat_o = nullptr, *wb_dat_i = nullptr, *wb_we = nullptr;
    Net *wb_sel = nullptr, *wb_stb = nullptr, *wb_cyc = nullptr, *wb_ack = nullptr, *halted = nullptr;

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
    void load(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        for (size_t i = 0; i < insts.size(); i++) {
            uint32_t x = insts[i];
            for (int b = 0; b < 4; b++)
                m[i * 4 + b] = (uint8_t)((x >> (8 * b)) & 0xFF);
        }
    }
    void run(int cycles) {
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
    void oneCycle() {
        circuit->setWire(clk->id(), 0);
        circuit->tick();
        circuit->setWire(clk->id(), 1);
        circuit->tick();
    }
    bool isHalted() { return circuit->getWire(halted->id())[0] != 0; }
};
} // namespace

TEST(WfiTest, HaltsAndWakesOnInterrupt) {
    Fixture f;
    // 0x00: lui x5, 0x10000  ; 0x04: addi x5,x5,16 -> 0x10010 是 mtvec
    // 先写 mtvec 指向 0x100，随后执行 WFI 停机
    f.load({
            0x10000293u, // addi x5, x0, 0x100（mtvec 指向内存内的地址）
            0x30529073u, // csrw mtvec, x5
            0x08000313u, // addi x6, x0, 0x80（mie.MTIE 是 bit7）
            0x30431073u, // csrw mie, x6
            0x10500073u, // wfi
            0x00A00293u, // addi x5, x0, 10（唤醒后应执行到这里）
            0x00100073u, // ebreak
    });
    f.run(20);
    EXPECT_EQ(f.cpu->mcause(), 0u) << "不应发生 trap";
    EXPECT_TRUE(f.cpu->halted()) << "CPU 内部应处于停机态";
    EXPECT_TRUE(f.isHalted()) << "无中断时 WFI 应停机（halted 引脚）";

    // 送一个机器定时器中断（mie.MTIE 未开也能唤醒 WFI）
    f.circuit->setWire(f.mti->id(), 1);
    f.oneCycle();
    f.oneCycle();
    EXPECT_FALSE(f.isHalted()) << "中断到来后应唤醒";
}
