// RISC-V CPU A-扩展测试: LR/SC, AMO* 原子指令
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <cstring>

using namespace dsc;

struct AmoTestFixture {
    std::unique_ptr<Circuit> circuit;
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk_net, *rst_net, *irq_net, *mti_net, *msi_net;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    Net *halted_net;

    AmoTestFixture() {
        circuit = std::make_unique<Circuit>();
        auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));
        auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mem_ptr.get();
        circuit->addComponent(std::move(mem_ptr));
        clk_net = circuit->createNet("clk"); rst_net = circuit->createNet("rst");
        irq_net = circuit->createNet("irq");
        mti_net = circuit->createNet("mti");
        msi_net = circuit->createNet("msi");
        wb_adr = circuit->createNet("wb_adr"); wb_dat_o = circuit->createNet("wb_dat_o");
        wb_dat_i = circuit->createNet("wb_dat_i"); wb_we = circuit->createNet("wb_we");
        wb_sel = circuit->createNet("wb_sel"); wb_stb = circuit->createNet("wb_stb");
        wb_cyc = circuit->createNet("wb_cyc"); wb_ack = circuit->createNet("wb_ack");
        halted_net = circuit->createNet("halted");
        circuit->connect(cpu, "clk", clk_net); circuit->connect(cpu, "rst", rst_net);
        circuit->connect(cpu, "irq", irq_net); circuit->connect(cpu, "mti", mti_net);
        circuit->connect(cpu, "msi", msi_net); circuit->connect(cpu, "wb_dat_i", wb_dat_i);
        circuit->connect(cpu, "wb_ack_i", wb_ack);
        circuit->connect(cpu, "wb_adr_o", wb_adr); circuit->connect(cpu, "wb_dat_o", wb_dat_o);
        circuit->connect(cpu, "wb_we_o", wb_we); circuit->connect(cpu, "wb_sel_o", wb_sel);
        circuit->connect(cpu, "wb_stb_o", wb_stb); circuit->connect(cpu, "wb_cyc_o", wb_cyc);
        circuit->connect(cpu, "halted", halted_net);
        circuit->connect(mem, "clk", clk_net); circuit->connect(mem, "wb_adr_i", wb_adr);
        circuit->connect(mem, "wb_dat_i", wb_dat_o); circuit->connect(mem, "wb_we_i", wb_we);
        circuit->connect(mem, "wb_sel_i", wb_sel); circuit->connect(mem, "wb_stb_i", wb_stb);
        circuit->connect(mem, "wb_cyc_i", wb_cyc); circuit->connect(mem, "wb_dat_o", wb_dat_i);
        circuit->connect(mem, "wb_ack_o", wb_ack);
    }

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

    void reset() {
        circuit->setWire(clk_net->id(), {0});
        circuit->setWire(rst_net->id(), {1});
        circuit->setWire(irq_net->id(), {0});
        circuit->tick();
        circuit->setWire(rst_net->id(), {0});
    }
};

// AMO 指令编码辅助: AMO opcode=0x2F, funct3=2(.W)
// 指令格式: funct5[31:27] aq[26] rl[25] rs2[24:20] rs1[19:15] funct3[14:12] rd[11:7] opcode[6:0]
static uint32_t amo_inst(uint32_t funct5, uint32_t rs2, uint32_t rs1, uint32_t rd) {
    return (funct5 << 27) | (rs2 << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x2F;
}

// ============================================================
// AMOSWAP: 原子交换
//   sw x1,0(x0) 存初值; amoswap.w x3,x2,(x0) 交换; lw x4,0(x0) 验证
// ============================================================
TEST(AmoTest, AmoSwap) {
    AmoTestFixture f;
    // 设置 mtvec 指向末尾 spin
    uint8_t *mp = f.mem->writableMemPtr();
    // 0x00: addi x1,x0,0xAB; sw x1,0(x0)  -- 存 0xAB 到 addr 0
    // 0x08: addi x2,x0,0xCD
    // 0x0C: amoswap.w x3,x2,(x0) -- 交换: x3=旧值, mem[0]=x2
    // 0x10: lw x4,0(x0) -- x4 = mem[0] = 0xCD
    // 0x14: ebreak (trap handler)
    // mtvec 初始化 + trap handler
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    uint32_t trap = 8 + 6*4; // mtvec_init(8B) + 6条指令(24B) = 0x20
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7)); // addi x1,x0,trap
    w32(4, 0x30509073); // csrrw x0, mtvec, x1
    w32(8, 0x0AB00093); // addi x1,x0,0xAB
    w32(12,0x00102023); // sw x1,0(x0)
    w32(16,0x0CD00113); // addi x2,x0,0xCD
    w32(20,amo_inst(1,2,0,3)); // amoswap.w x3,x2,(x0) — funct5=1
    w32(24,0x00002203); // lw x4,0(x0)
    w32(28,0x00100073); // ebreak
    w32(trap, 0x0000006F); // trap handler: spin

    f.compile();
    f.reset();
    f.runCycles(120);

    EXPECT_EQ(f.cpu->reg(3), 0xABu);  // AMOSWAP 返回旧值
    EXPECT_EQ(f.cpu->reg(4), 0xCDu);  // 新值写入内存
}

// ============================================================
// AMOADD: 原子加
// ============================================================
TEST(AmoTest, AmoAdd) {
    AmoTestFixture f;
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    uint32_t trap = 8 + 5*4;
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7));
    w32(4, 0x30509073);
    w32(8, 0x00A00093); // addi x1,x0,10
    w32(12,0x00102023); // sw x1,0(x0) — mem[0]=10
    w32(16,0x00500113); // addi x2,x0,5
    w32(20,amo_inst(0,2,0,3)); // amoadd.w x3,x2,(x0) — funct5=0 (ADD)
    w32(24,0x00100073); // ebreak
    w32(trap, 0x0000006F);

    f.compile();
    f.reset();
    f.runCycles(100);

    // amoadd: 读旧值(10), 加 5, 写回 15. reg[3]=旧值=10
    EXPECT_EQ(f.cpu->reg(3), 10u);
    // 验证内存内容：mem[0] 应为 15 (10 + 5)
    uint8_t *mem = f.mem->writableMemPtr();
    uint32_t mem_val = mem[0] | (mem[1] << 8) | (mem[2] << 16) | (mem[3] << 24);
    EXPECT_EQ(mem_val, 15u);
}

// ============================================================
// LR/SC: 保留加载 + 条件存储
// ============================================================
TEST(AmoTest, LR_SC_Success) {
    AmoTestFixture f;
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    // LR.W 读取, SC.W 写入 (相同地址 → 应成功)
    uint32_t trap = 8 + 7*4;
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7));
    w32(4, 0x30509073);
    w32(8, 0x0FF00093); // addi x1,x0,0xFF
    w32(12,0x00102023); // sw x1,0(x0) — mem[0]=0xFF
    w32(16,amo_inst(2,0,0,1)); // lr.w x1,(x0) — funct5=2(LR), x1=旧值=0xFF
    w32(20,0x00500113); // addi x2,x0,5
    w32(24,amo_inst(3,2,0,3)); // sc.w x3,x2,(x0) — funct5=3(SC), x3=0(成功)
    w32(28,0x00002203); // lw x4,0(x0) — x4 = mem[0] = 5
    w32(32,0x00100073); // ebreak
    w32(trap, 0x0000006F);

    f.compile();
    f.reset();
    f.runCycles(150);

    EXPECT_EQ(f.cpu->reg(1), 0xFFu); // LR 返回旧值
    EXPECT_EQ(f.cpu->reg(3), 0u);    // SC 返回 0 (成功)
    EXPECT_EQ(f.cpu->reg(4), 5u);    // 内存被 SC 写入 5
}

// ============================================================
// SC 失败: SC 到不同地址 → 应失败
// ============================================================
TEST(AmoTest, SC_Fail) {
    AmoTestFixture f;
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    // LR from addr 0, SC to addr 4 → 不同地址，SC 应失败
    uint32_t trap = 8 + 6*4;
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7));
    w32(4, 0x30509073);
    w32(8, 0x0FF00093); // addi x1,x0,0xFF
    w32(12,0x00102023); // sw x1,0(x0) — mem[0]=0xFF
    w32(16,amo_inst(2,0,0,1)); // lr.w x1,(x0) — 保留 adr=0
    w32(20,0x00500113); // addi x2,x0,5
    // SC 到 addr 4 (rs1=x3? 用 x5=4 做 base)
    w32(24,0x00400293); // addi x5,x0,4
    w32(28,amo_inst(3,2,5,3)); // sc.w x3,x2,(x5) — 不同地址，应失败→x3=1
    w32(32,0x00100073); // ebreak
    w32(trap, 0x0000006F);

    f.compile();
    f.reset();
    f.runCycles(150);

    EXPECT_EQ(f.cpu->reg(1), 0xFFu); // LR 返回旧值
    EXPECT_EQ(f.cpu->reg(3), 1u);    // SC 返回非零 (失败)
}

// ============================================================
// AMOAND: 原子按位与
// ============================================================
TEST(AmoTest, AmoAnd) {
    AmoTestFixture f;
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    // mem[0]=0xFF, amoand.w x3,x2,(x0) with x2=0x0F → mem[0]=0x0F, x3=0xFF
    uint32_t trap = 8 + 5*4;
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7));
    w32(4, 0x30509073);                         // csrrw x0, mtvec, x1
    w32(8, 0x0FF00093);                         // addi x1,x0,0xFF
    w32(12,0x00102023);                         // sw x1,0(x0) — mem[0]=0xFF
    w32(16,0x00F00113);                         // addi x2,x0,0xF
    w32(20,amo_inst(0x0C,2,0,3));               // amoand.w x3,x2,(x0) — funct5=0x0C (AND)
    w32(24,0x00100073);                         // ebreak
    w32(trap, 0x0000006F);

    f.compile();
    f.reset();
    f.runCycles(100);

    EXPECT_EQ(f.cpu->reg(3), 0xFFu);            // 返回旧值
    uint8_t *mem = f.mem->writableMemPtr();
    uint32_t mem_val = mem[0] | (mem[1] << 8) | (mem[2] << 16) | (mem[3] << 24);
    EXPECT_EQ(mem_val, 0x0Fu);                  // 0xFF & 0x0F = 0x0F
}

// ============================================================
// AMOOR: 原子按位或
// ============================================================
TEST(AmoTest, AmoOr) {
    AmoTestFixture f;
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    // mem[0]=0x0F, amoor.w x3,x2,(x0) with x2=0xF0 → mem[0]=0xFF, x3=0x0F
    uint32_t trap = 8 + 5*4;
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7));
    w32(4, 0x30509073);
    w32(8, 0x00F00093);                         // addi x1,x0,0xF
    w32(12,0x00102023);                         // sw x1,0(x0) — mem[0]=0x0F
    w32(16,0x0F000113);                         // addi x2,x0,0xF0
    w32(20,amo_inst(0x08,2,0,3));               // amoor.w x3,x2,(x0) — funct5=0x08 (OR)
    w32(24,0x00100073);                         // ebreak
    w32(trap, 0x0000006F);

    f.compile();
    f.reset();
    f.runCycles(100);

    EXPECT_EQ(f.cpu->reg(3), 0x0Fu);            // 返回旧值
    uint8_t *mem = f.mem->writableMemPtr();
    uint32_t mem_val = mem[0] | (mem[1] << 8) | (mem[2] << 16) | (mem[3] << 24);
    EXPECT_EQ(mem_val, 0xFFu);                  // 0x0F | 0xF0 = 0xFF
}

// ============================================================
// AMO 连续操作：两次 AMO 对同一地址，验证原子性
// ============================================================
TEST(AmoTest, SequentialAtomicOps) {
    AmoTestFixture f;
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    // mem[0]=5 → amoadd +3 → mem[0]=8 → amoswap x0=old=8, x2=99 → mem[0]=99
    uint32_t trap = 8 + 7*4;
    w32(0, 0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7));
    w32(4, 0x30509073);                         // csrrw x0, mtvec, x1
    w32(8, 0x00500093);                         // addi x1,x0,5
    w32(12,0x00102023);                         // sw x1,0(x0) — mem[0]=5
    w32(16,0x00300113);                         // addi x2,x0,3
    w32(20,amo_inst(0,2,0,3));                  // amoadd.w x3,x2,(x0) — x3=5, mem[0]=8
    w32(24,0x06300113);                         // addi x2,x0,99
    w32(28,amo_inst(1,2,0,4));                  // amoswap.w x4,x2,(x0) — x4=8, mem[0]=99
    w32(32,0x00100073);                         // ebreak
    w32(trap, 0x0000006F);

    f.compile();
    f.reset();
    f.runCycles(120);

    EXPECT_EQ(f.cpu->reg(3), 5u);               // amoadd 返回旧值 5
    EXPECT_EQ(f.cpu->reg(4), 8u);               // amoswap 返回旧值 8
    uint8_t *mem = f.mem->writableMemPtr();
    uint32_t mem_val = mem[0] | (mem[1] << 8) | (mem[2] << 16) | (mem[3] << 24);
    EXPECT_EQ(mem_val, 99u);                     // 最终 mem[0]=99
}
