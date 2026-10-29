// RISC-V CPU + WishboneMemory 测试
//
// 每个测试程序自动在开头设 mtvec 指向末尾的自旋陷阱处理程序，
// ecall/ebreak/非法指令 都会跳到该处理程序自旋，不会停机。
// 测试通过检查 PC == trap_addr 验证程序正常结束。
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>
#include <cstring>

using namespace dsc;

struct CpuTestFixture {
    std::unique_ptr<Circuit> circuit;
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk_net, *rst_net, *irq_net, *mti_net, *msi_net;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    Net *halted_net;

    CpuTestFixture() {
        circuit = std::make_unique<Circuit>();

        auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));

        auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mem_ptr.get();
        circuit->addComponent(std::move(mem_ptr));

        clk_net = circuit->createNet("clk");
        rst_net = circuit->createNet("rst");
        irq_net = circuit->createNet("irq");
        mti_net = circuit->createNet("mti");
        msi_net = circuit->createNet("msi");
        wb_adr = circuit->createNet("wb_adr");
        wb_dat_o = circuit->createNet("wb_dat_o");
        wb_dat_i = circuit->createNet("wb_dat_i");
        wb_we = circuit->createNet("wb_we");
        wb_sel = circuit->createNet("wb_sel");
        wb_stb = circuit->createNet("wb_stb");
        wb_cyc = circuit->createNet("wb_cyc");
        wb_ack = circuit->createNet("wb_ack");
        halted_net = circuit->createNet("halted");

        circuit->connect(cpu, "clk", clk_net);
        circuit->connect(cpu, "rst", rst_net);
        circuit->connect(cpu, "irq", irq_net);
        circuit->connect(cpu, "mti", mti_net);
        circuit->connect(cpu, "msi", msi_net);
        circuit->connect(cpu, "wb_dat_i", wb_dat_i);
        circuit->connect(cpu, "wb_ack_i", wb_ack);
        circuit->connect(cpu, "wb_adr_o", wb_adr);
        circuit->connect(cpu, "wb_dat_o", wb_dat_o);
        circuit->connect(cpu, "wb_we_o", wb_we);
        circuit->connect(cpu, "wb_sel_o", wb_sel);
        circuit->connect(cpu, "wb_stb_o", wb_stb);
        circuit->connect(cpu, "wb_cyc_o", wb_cyc);
        circuit->connect(cpu, "halted", halted_net);

        circuit->connect(mem, "clk", clk_net);
        circuit->connect(mem, "wb_adr_i", wb_adr);
        circuit->connect(mem, "wb_dat_i", wb_dat_o);
        circuit->connect(mem, "wb_we_i", wb_we);
        circuit->connect(mem, "wb_sel_i", wb_sel);
        circuit->connect(mem, "wb_stb_i", wb_stb);
        circuit->connect(mem, "wb_cyc_i", wb_cyc);
        circuit->connect(mem, "wb_dat_o", wb_dat_i);
        circuit->connect(mem, "wb_ack_o", wb_ack);
    }

    // 加载程序 + 自动设 mtvec 指向末尾自旋陷阱处理程序
    // 返回陷阱处理程序地址（用于测试断言 PC）
    uint32_t loadAndSetup(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        size_t N = insts.size();

        // 陷阱地址 = mtvec_init(8B) + 用户程序(N*4B)
        uint32_t trap_addr = (uint32_t)(8 + N * 4);

        auto w = [&](size_t off, uint32_t inst) {
            m[off + 0] = (uint8_t)(inst & 0xFF);
            m[off + 1] = (uint8_t)((inst >> 8) & 0xFF);
            m[off + 2] = (uint8_t)((inst >> 16) & 0xFF);
            m[off + 3] = (uint8_t)((inst >> 24) & 0xFF);
        };

        // addi x1, x0, trap_addr  (低 12 位)
        w(0, 0x00000093 | ((trap_addr & 0xFFF) << 20) | (1 << 7));
        // csrrw x0, mtvec, x1
        w(4, 0x30509073);
        // 用户程序
        for (size_t i = 0; i < N; i++)
            w(8 + i * 4, insts[i]);
        // 陷阱处理程序: jal x0, 0  (自旋)
        w(8 + N * 4, 0x0000006F);

        return trap_addr;
    }

    // 裸加载（不设 mtvec），用于需要手动控制内存布局的测试
    void loadProgram(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        for (size_t i = 0; i < insts.size(); i++) {
            uint32_t inst = insts[i];
            m[i * 4 + 0] = (uint8_t)(inst & 0xFF);
            m[i * 4 + 1] = (uint8_t)((inst >> 8) & 0xFF);
            m[i * 4 + 2] = (uint8_t)((inst >> 16) & 0xFF);
            m[i * 4 + 3] = (uint8_t)((inst >> 24) & 0xFF);
        }
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
    }

    void runCycles(int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk_net->id(), {0});
            circuit->tick();
            circuit->setWire(clk_net->id(), {1});
            circuit->tick();
        }
    }
};

// ============================================================
// 基本 ALU 指令
// ============================================================
TEST(RiscVCPUTest, BasicALU) {
    CpuTestFixture f;
    uint32_t trap = f.loadAndSetup({
            0x02A00093, // addi x1, x0, 42
            0x00A00113, // addi x2, x0, 10
            0x002081B3, // add  x3, x1, x2
            0x00100073, // ebreak → 陷阱
    });
    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    f.runCycles(40);

    EXPECT_EQ(f.cpu->reg(1), 42);
    EXPECT_EQ(f.cpu->reg(2), 10);
    EXPECT_EQ(f.cpu->reg(3), 52);
    EXPECT_EQ(f.cpu->pc(), trap);
}

// ============================================================
// 访存指令 (SW / LW)
// ============================================================
TEST(RiscVCPUTest, MemoryAccess) {
    CpuTestFixture f;
    uint32_t trap = f.loadAndSetup({
            0x0AB00093, // addi x1, x0, 0xAB
            0x00102023, // sw   x1, 0(x0)
            0x00002103, // lw   x2, 0(x0)
            0x00100073, // ebreak → 陷阱
    });
    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    f.runCycles(50);

    EXPECT_EQ(f.cpu->reg(1), 0xAB);
    EXPECT_EQ(f.cpu->reg(2), 0xAB);
    EXPECT_EQ(f.cpu->pc(), trap);

    const uint8_t *mp = f.mem->readOnlyMemPtr();
    // 用户程序从偏移 8 开始；SW 写入用户程序区外的地址 0
    // 注意: 用户程序在偏移 8 处，但 SW 指令写入 addr=0，是 mtvec init 代码区
    EXPECT_EQ(mp[0], 0xAB);
}

// ============================================================
// 分支指令
// ============================================================
TEST(RiscVCPUTest, Branch) {
    CpuTestFixture f;
    uint32_t trap = f.loadAndSetup({
            0x00500093, // addi x1, x0, 5
            0x00500113, // addi x2, x0, 5
            0x00208463, // beq  x1, x2, 8    (跳过下一条)
            0x06300193, // addi x3, x0, 99   (被跳过)
            0x00100073, // ebreak → 陷阱
    });
    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    f.runCycles(50);

    EXPECT_EQ(f.cpu->reg(3), 0);  // 分支跳过了 addi x3
    EXPECT_EQ(f.cpu->pc(), trap);
}

// ============================================================
// JAL 指令
// ============================================================
TEST(RiscVCPUTest, JumpAndLink) {
    CpuTestFixture f;
    uint32_t trap = f.loadAndSetup({
            0x008000EF, // jal  x1, 8       (x1 = ret, skip next)
            0x06300113, // addi x2, x0, 99  (跳过)
            0x04D00193, // addi x3, x0, 77
            0x00100073, // ebreak → 陷阱
    });
    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    f.runCycles(50);

    EXPECT_EQ(f.cpu->reg(1), 12); // 返回地址 (用户程序内偏移)
    EXPECT_EQ(f.cpu->reg(2), 0);  // 被跳过
    EXPECT_EQ(f.cpu->reg(3), 77);
    EXPECT_EQ(f.cpu->pc(), trap);
}

// ============================================================
// CSR 读写
// ============================================================
TEST(RiscVCPUTest, CSRReadWrite) {
    CpuTestFixture f;
    uint32_t trap = f.loadAndSetup({
            0x08800093, // addi x1, x0, 0x88
            0x30409073, // csrrw x0, mie, x1     (写 mie=0x88)
            0x30402173, // csrrs x2, mie, x0     (读 mie → x2)
            0x00100073, // ebreak → 陷阱
    });
    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->setWire(f.irq_net->id(), {0});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    f.runCycles(50);

    EXPECT_EQ(f.cpu->reg(2), 0x88u); // csrrs 读回的 mtvec 旧值
    EXPECT_EQ(f.cpu->pc(), trap);
}

// ============================================================
// 外部中断 陷阱入口 + mret 返回
// ============================================================
TEST(RiscVCPUTest, ExternalInterrupt) {
    CpuTestFixture f;
    // mtvec 会被 loadAndSetup 初始化为末尾自旋; 但我们需要 mtvec=0x100
    // 所以用 loadProgram 手动控制布局
    f.loadProgram({
            // 0x00: addi x1,x0,0x100; csrrw x0,mtvec,x1  (重设 mtvec=0x100)
            0x10000093, // addi x1, x0, 0x100
            0x30509073, // csrrw x0, mtvec, x1
            // 0x08: mstatus.MIE
            0x00800093, // addi x1, x0, 8
            0x30009073, // csrrw x0, mstatus, x1
            // 0x10: mie.MEIE
            0x00100093, // addi x1, x0, 1
            0x00B09093, // slli x1, x1, 11
            0x30409073, // csrrw x0, mie, x1
            // 0x1C: jal x0, 0 (自旋等待中断)
            0x0000006F,
    });
    // 填充 0x20..0xFF → nop, 0x100 → 中断处理程序
    auto *m = f.mem->writableMemPtr();
    for (int i = 8; i < 64; i++) {
        uint32_t nop = 0x00000013; size_t off = i * 4;
        m[off + 0] = nop & 0xFF; m[off + 1] = (nop >> 8) & 0xFF;
        m[off + 2] = (nop >> 16) & 0xFF; m[off + 3] = (nop >> 24) & 0xFF;
    }
    // 0x100: csrrs x2, mcause, x0
    uint32_t inst1 = 0x34202173;
    m[256 + 0] = inst1 & 0xFF; m[256 + 1] = (inst1 >> 8) & 0xFF;
    m[256 + 2] = (inst1 >> 16) & 0xFF; m[256 + 3] = (inst1 >> 24) & 0xFF;
    // 0x104: mret
    uint32_t inst2 = 0x30200073;
    m[260 + 0] = inst2 & 0xFF; m[260 + 1] = (inst2 >> 8) & 0xFF;
    m[260 + 2] = (inst2 >> 16) & 0xFF; m[260 + 3] = (inst2 >> 24) & 0xFF;

    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->setWire(f.irq_net->id(), {0});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});

    f.runCycles(60);
    f.circuit->setWire(f.irq_net->id(), {1});
    f.runCycles(40);

    EXPECT_EQ(f.cpu->reg(2), 0x8000000Bu);
}

// ============================================================
// 陷阱 + mret 测试: ebreak → handler(set mepc=0x0C) → mret
// ============================================================
TEST(RiscVCPUTest, TrapMret) {
    CpuTestFixture f;
    // mtvec = 0x14 (trap handler at 0x14)
    // 0x08: ebreak → trap to 0x14
    // Handler: addi x2,0x0C; csrrw x0,mepc,x2; mret → return to 0x0C
    // 0x0C: addi x3,99; jal x0,0 (spin)
    f.loadProgram({
        0x01400093, // 0x00: addi x1,x0,0x14
        0x30509073, // 0x04: csrrw x0, mtvec, x1
        0x00100073, // 0x08: ebreak
        0x06300193, // 0x0C: addi x3,x0,99
        0x0000006F, // 0x10: jal x0,0
        0x00C00113, // 0x14: addi x2,x0,12
        0x34111073, // 0x18: csrrw x0, mepc, x2
        0x30200073, // 0x1C: mret
    });
    f.compile();
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->setWire(f.irq_net->id(), {0});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    f.runCycles(80);

    EXPECT_EQ(f.cpu->reg(3), 99);
    EXPECT_EQ(f.cpu->pc(), 0x10u);
}

// ============================================================
