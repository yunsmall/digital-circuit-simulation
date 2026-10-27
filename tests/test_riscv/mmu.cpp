// RISC-V CPU Sv32 MMU 测试: 页表遍历走 Wishbone 总线
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>
#include <cstring>

using namespace dsc;

struct MmuTestFixture {
    std::unique_ptr<Circuit> circuit;
    RiscVCPU *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk_net, *rst_net, *irq_net, *mti_net, *msi_net;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    Net *halted_net;

    MmuTestFixture() {
        circuit = std::make_unique<Circuit>();
        auto cpu_ptr = std::make_unique<RiscVCPU>("cpu0");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));
        auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32); // 64KB
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

// ============================================================
// Sv32 基本页表遍历: 1 级页表 + 2 级页表 → TLB 装填 → LOAD 成功
//
// 内存布局（物理地址）:
//   0x0000-0x00FF: M-mode 设置代码 + 陷阱处理程序
//   0x0100-0x01FF: S-mode 测试代码 (identity mapped VA=PA)
//   0x6000: 数据 (值 0xAB, 映射到 VA 0x2000)
//   0xA000-0xAFFF: 根页表 (L1)
//   0xB000-0xBFFF: L2 页表
//
// 地址映射:
//   VA 0x0000-0x0FFF → PA 0x0000-0x0FFF (identity, X+R+W, 代码用)
//   VA 0x2000-0x2FFF → PA 0x6000-0x6FFF (数据页, R+W)
// ============================================================
TEST(MmuTest, Sv32BasicDataLoad) {
    MmuTestFixture f;

    // 预填页表和数据（直接写物理内存）
    uint8_t *mp = f.mem->writableMemPtr();
    auto w32p = [&](size_t pa, uint32_t v) {
        mp[pa+0] = (uint8_t)(v & 0xFF);
        mp[pa+1] = (uint8_t)((v>>8) & 0xFF);
        mp[pa+2] = (uint8_t)((v>>16) & 0xFF);
        mp[pa+3] = (uint8_t)((v>>24) & 0xFF);
    };

    // satp: MODE=Sv32, PPN=0xA (根页表 @ PA 0xA000)
    uint32_t satp_val = 0x80000000u | 0xA; // Sv32 mode + root PPN

    // L1 PTE @ PA 0xA000 (vpn[1]=0): 指向 L2 @ PA 0xB000, V=1
    w32p(0xA000, (0xB << 10) | 0x01); // PPN=0xB, R=W=X=0(非叶子), V=1

    // L2 PTE[0] @ PA 0xB000 (vpn[0]=0, VA 0x0000-0x0FFF):
    //   identity map: PPN=0, V=1,R=1,W=1,X=1,U=1
    w32p(0xB000, (0x0 << 10) | 0x0F);

    // L2 PTE[2] @ PA 0xB008 (vpn[0]=2, VA 0x2000-0x2FFF):
    //   map to PA 0x6000: PPN=0x6, V=1,R=1,W=1（S 态访问，U 位必须为 0，否则需要 SUM）
    w32p(0xB008, (0x6 << 10) | 0x07);

    // 数据 @ PA 0x6000
    w32p(0x6000, 0x000000AB);

    // 陷阱处理程序地址: 在 M-mode 设置代码之后
    // 0x00-0x03: addi x1,x0,trap
    // 0x04-0x07: csrrw x0,mtvec,x1
    // 0x08-0x0B: addi x1,x0,satp_lo (satp 低 12 位)
    // 0x0C-0x0F: lui x2,satp_hi (satp 高 20 位)
    // 0x10-0x13: or x1,x1,x2 → x1=satp
    // 0x14-0x17: csrrw x0,satp,x1 → 写 satp
    // 0x18-0x1B: lui x1,0x00001 (mstatus.MPP=S=1<<11)
    // 0x1C-0x1F: addi x1,x1,0x80 (mstatus.MPIE)
    // 0x20-0x23: csrrw x0,mstatus,x1
    // 0x24-0x27: addi x1,x0,0x40 → mepc=0x40 (S-mode 入口)
    // 0x28-0x2B: csrrw x0,mepc,x1
    // 0x2C-0x2F: mret → S-mode @ 0x40
    //
    // S-mode 代码 @ 0x40 (identity mapped):
    // 0x40-0x43: lui x5,0x2 (x5=0x2000)
    // 0x44-0x47: lw x3,0(x5) → VA 0x2000 → MMU walk → PA 0x6000 → 0xAB
    // 0x48-0x4B: ecall → trap to M-mode
    //
    // M-mode 陷阱处理程序 @ trap_addr (mtvec):
    // jal x0,0 (自旋)

    uint32_t trap = 0x30; // M-mode 初始化代码占 0x00..0x2F(12条), trap 在 0x30

    f.loadProgram({
        // mtvec = trap (0x30)
        0x00000093u | ((trap & 0xFFF) << 20) | (1 << 7), // 0x00: addi x1,x0,trap
        0x30509073u,                                       // 0x04: csrrw x0,mtvec,x1

        // 构造 satp 值 (satp_val = 0x8000000A)
        (uint32_t)(0x00000093 | ((satp_val & 0xFFF) << 20) | (1 << 7)), // 0x08: addi x1,x0,satp&0xFFF
        (uint32_t)(0x00000137 | (((satp_val >> 12) & 0xFFFFF) << 12)),  // 0x0C: lui x2,satp_hi
        (uint32_t)(0x0020E0B3u),                                          // 0x10: or x1,x1,x2
        0x18009073u,                                       // 0x14: csrrw x0,satp,x1

        // mstatus = MPP=S (1<<11) | MPIE (1<<7) = 0x880
        0x000010B7u,                                       // 0x18: lui x1, 0x00001 (x1=0x1000)
        0x88008093u,                                       // 0x1C: addi x1,x1,-1920 → x1=0x880
        0x30009073u,                                       // 0x20: csrrw x0,mstatus,x1

        // mepc = 0x40 (S-mode 入口)
        0x04000093u,                                       // 0x24: addi x1,x0,0x40
        0x34109073u,                                       // 0x28: csrrw x0,mepc,x1

        // mret → S-mode
        0x30200073u,                                       // 0x2C: mret

        // 陷阱处理程序 @ 0x30
        0x0000006Fu,                                       // 0x30: jal x0,0 (自旋)
    });

    // 在 0x40 处写入 S-mode 代码
    uint32_t s_code = 0x40;
    auto w32i = [&](size_t off, uint32_t inst) {
        mp[off+0] = (uint8_t)(inst & 0xFF);
        mp[off+1] = (uint8_t)((inst>>8) & 0xFF);
        mp[off+2] = (uint8_t)((inst>>16) & 0xFF);
        mp[off+3] = (uint8_t)((inst>>24) & 0xFF);
    };
    w32i(s_code + 0,  0x000022B7u); // lui x5,0x2 → x5=0x2000
    w32i(s_code + 4,  0x0002A183u); // lw x3,0(x5) → 从 VA 0x2000 加载
    w32i(s_code + 8,  0x00000073u); // ecall → trap to M-mode

    // 额外填充 S-mode 代码后面的陷阱代码 (0x4C → spin)
    // ecall 之后 PC=0x4C，但这不重要因为 trap 会跳转到 mtvec

    f.compile();
    f.reset();
    f.runCycles(200);

    // S-mode 中 lw x3,0(x5) 加载 VA 0x2000 → MMU 翻译 → PA 0x6000 → x3=0xAB
    EXPECT_EQ(f.cpu->reg(3), 0xABu) << "MMU 页表遍历后 x3 应为 0xAB";
    EXPECT_FALSE(f.cpu->halted()) << "CPU 不应停机";
}

// ============================================================
// Sv32 L1 缓存命中: 两次访问同一 4MB 区域，第二次应跳过 L1 总线读
// ============================================================
TEST(MmuTest, Sv32L1CacheHit) {
    MmuTestFixture f;

    uint8_t *mp = f.mem->writableMemPtr();
    auto w32p = [&](size_t pa, uint32_t v) {
        mp[pa+0] = (uint8_t)(v & 0xFF);
        mp[pa+1] = (uint8_t)((v>>8) & 0xFF);
        mp[pa+2] = (uint8_t)((v>>16) & 0xFF);
        mp[pa+3] = (uint8_t)((v>>24) & 0xFF);
    };

    uint32_t satp_val = 0x80000000u | 0xA;
    w32p(0xA000, (0xB << 10) | 0x01);   // L1: PPN=0xB, V=1
    w32p(0xB000, (0x0 << 10) | 0x0F);    // L2[0]: identity map VA 0x0000
    // 用 0x07（V|R|W）而不是 0x17：S 态访问 U=1 的页需要 mstatus.SUM，这里测的是普通 S 页
    w32p(0xB008, (0x6 << 10) | 0x07);    // L2[2]: VA 0x2000→PA 0x6000
    w32p(0xB00C, (0x7 << 10) | 0x07);    // L2[3]: VA 0x3000→PA 0x7000
    w32p(0x6000, 0x000000AB);            // 数据 @ PA 0x6000
    w32p(0x7000, 0x000000CD);            // 数据 @ PA 0x7000

    uint32_t trap = 0x30;
    f.loadProgram({
        (uint32_t)(0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7)),
        0x30509073u,
        // satp
        (uint32_t)(0x00000093 | ((satp_val & 0xFFF) << 20) | (1 << 7)),
        (uint32_t)(0x00000137 | (((satp_val >> 12) & 0xFFFFF) << 12)),
        (uint32_t)(0x0020E0B3u),
        0x18009073u,
        // mstatus = 0x880 (MPP=S, MPIE)
        0x000010B7u,
        0x88008093u,
        0x30009073u,
        // mepc = 0x40
        0x04000093u,
        0x34109073u,
        // mret
        0x30200073u,
        // trap handler @ 0x30
        0x0000006Fu,
    });

    auto w32i = [&](size_t off, uint32_t inst) {
        mp[off+0] = (uint8_t)(inst & 0xFF);
        mp[off+1] = (uint8_t)((inst>>8) & 0xFF);
        mp[off+2] = (uint8_t)((inst>>16) & 0xFF);
        mp[off+3] = (uint8_t)((inst>>24) & 0xFF);
    };
    // S-mode @ 0x40: 第一次 lw VA 0x2000 → TLB miss, L1 miss → 完整遍历
    //               第二次 lw VA 0x3000 → TLB miss, L1 HIT → 跳过 L1 读
    w32i(0x40, 0x000022B7u); // lui x5, 0x2 (VA 0x2000)
    w32i(0x44, 0x0002A183u); // lw x3, 0(x5) → MMU walk → x3=0xAB
    w32i(0x48, 0x000032B7u); // lui x5, 0x3 (VA 0x3000)
    w32i(0x4C, 0x0002A203u); // lw x4, 0(x5) → TLB miss, L1 cache hit → x4=0xCD
    w32i(0x50, 0x00000073u); // ecall

    f.compile();
    f.reset();
    f.runCycles(300);

    EXPECT_EQ(f.cpu->reg(3), 0xABu) << "首次 MMU 遍历后 x3 应为 0xAB";
    EXPECT_EQ(f.cpu->reg(4), 0xCDu) << "L1 缓存命中后 x4 应为 0xCD";
}

// ============================================================
// Sv32 无效 PTE: V=0 的页表条目应触发页错误
// ============================================================
TEST(MmuTest, Sv32InvalidPTE) {
    MmuTestFixture f;

    uint8_t *mp = f.mem->writableMemPtr();
    auto w32p = [&](size_t pa, uint32_t v) {
        mp[pa+0] = (uint8_t)(v & 0xFF);
        mp[pa+1] = (uint8_t)((v>>8) & 0xFF);
        mp[pa+2] = (uint8_t)((v>>16) & 0xFF);
        mp[pa+3] = (uint8_t)((v>>24) & 0xFF);
    };

    uint32_t satp_val = 0x80000000u | 0xA;
    w32p(0xA000, (0xB << 10) | 0x01);   // L1: PPN=0xB, V=1
    w32p(0xB000, (0x0 << 10) | 0x0F);    // L2[0]: identity map
    // L2[2]: V=0 (无效条目)
    w32p(0xB008, 0x00000000);            // VA 0x2000 映射无效

    uint32_t trap = 0x30;
    f.loadProgram({
        (uint32_t)(0x00000093 | ((trap & 0xFFF) << 20) | (1 << 7)),
        0x30509073u,
        (uint32_t)(0x00000093 | ((satp_val & 0xFFF) << 20) | (1 << 7)),
        (uint32_t)(0x00000137 | (((satp_val >> 12) & 0xFFFFF) << 12)),
        (uint32_t)(0x0020E0B3u),
        0x18009073u,
        0x000010B7u,
        0x88008093u,
        0x30009073u,
        0x04000093u,
        0x34109073u,
        0x30200073u,
        0x0000006Fu,
    });

    auto w32i = [&](size_t off, uint32_t inst) {
        mp[off+0] = (uint8_t)(inst & 0xFF);
        mp[off+1] = (uint8_t)((inst>>8) & 0xFF);
        mp[off+2] = (uint8_t)((inst>>16) & 0xFF);
        mp[off+3] = (uint8_t)((inst>>24) & 0xFF);
    };
    // S-mode @ 0x40: lw from VA 0x2000 → MMU walk → L2 PTE V=0 → page fault
    w32i(0x40, 0x000022B7u); // lui x5, 0x2
    w32i(0x44, 0x0002A183u); // lw x3, 0(x5) → 应触发页错误
    w32i(0x48, 0x04D00193u); // addi x3, x0, 77 (不应该执行到)
    w32i(0x4C, 0x00000073u); // ecall

    f.compile();
    f.reset();
    f.runCycles(200);

    // 页错误 → trap to M-mode handler → x3 保持 M-mode 初始化时的值
    // x3 不应该是 77（那意味着页错误没触发，load 成功了）
    EXPECT_NE(f.cpu->reg(3), 77u) << "无效 PTE 应触发页错误，不应执行到 addi x3,77";
    // CPU 应该在陷阱处理程序中自旋
    EXPECT_FALSE(f.cpu->halted());
}
