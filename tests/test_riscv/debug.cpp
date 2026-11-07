// 最小调试测试：验证 JIT 回调 + 单指令执行
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include "common/wb_cpu_link.h"
#include <dcs/components/memory/WishboneMemory.h>
#include <cstring>
#include <iostream>

using namespace dsc;

TEST(RiscVCPUDebug, SingleAddiInstruction) {
    Circuit c;

    // 创建 CPU 和内存
    auto *cpu = static_cast<RiscVCPU<32>*>(c.addComponent(std::make_unique<RiscVCPU<32>>("cpu0")));
    auto link = test::linkCpuToWb(c, cpu);
    auto *mem = static_cast<WishboneMemory*>(c.addComponent(std::make_unique<WishboneMemory>("mem0", 12, 32)));

    // 加载单条指令: addi x1, x0, 42
    uint32_t inst = 0x02A00093;
    uint8_t *m = mem->writableMemPtr();
    m[0] = inst & 0xFF;
    m[1] = (inst >> 8) & 0xFF;
    m[2] = (inst >> 16) & 0xFF;
    m[3] = (inst >> 24) & 0xFF;

    // 创建并连接线网
    auto *clk = c.createNet("clk");
    auto *rst = c.createNet("rst");
    auto *irq_net = c.createNet("irq");
    auto *wb_adr = link.adr;
    auto *wb_do = link.dat_o;
    auto *wb_di = link.dat_i;
    auto *wb_we = link.we;
    auto *wb_sel = link.sel;
    auto *wb_stb = link.stb;
    auto *wb_cyc = link.cyc;
    auto *wb_ack = link.ack;
    auto *halt = c.createNet("halt");

    // CPU 输入
    c.connect(cpu, "clk", clk);
    c.connect(cpu, "rst", rst);
    c.connect(cpu, "irq", irq_net);
    c.connect(cpu, "halted", halt);

    // 内存输入
    c.connect(mem, "wb_adr_i", wb_adr);
    c.connect(mem, "wb_dat_i", wb_do);
    c.connect(mem, "wb_we_i", wb_we);
    c.connect(mem, "wb_sel_i", wb_sel);
    c.connect(mem, "wb_stb_i", wb_stb);
    c.connect(mem, "wb_cyc_i", wb_cyc);
    c.connect(mem, "clk", clk);
    // 内存输出
    c.connect(mem, "wb_dat_o", wb_di);
    c.connect(mem, "wb_ack_o", wb_ack);

    // 打印生成的 C 代码（调试用）
    std::cout << "=== GENERATED C CODE ===" << std::endl;
    std::cout << c.generateC() << std::endl;
    std::cout << "=== END C CODE ===" << std::endl;

    // 编译
    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;

    c.init();

    // 复位
    c.setWire(clk->id(), 0);
    c.setWire(rst->id(), 1);
    c.tick();
    c.setWire(rst->id(), 0);
    std::cout << "After reset: pc=" << cpu->pc() << " halted=" << cpu->halted() << std::endl;

    // 运行若干个周期并打印状态
    for (int cycle = 0; cycle < 20; cycle++) {
        c.setWire(clk->id(), 0);
        c.tick();
        c.setWire(clk->id(), 1);
        c.tick();

        if (cycle < 5 || cpu->halted()) {
            std::cout << "Cycle " << cycle << ": pc=" << cpu->pc()
                      << " reg1=" << cpu->reg(1)
                      << " halted=" << cpu->halted()
                      << std::endl;
        }
        if (cpu->halted()) break;
    }

    std::cout << "Final: pc=" << cpu->pc() << " reg1=" << cpu->reg(1) << std::endl;
    EXPECT_EQ(cpu->reg(1), 42);
}
