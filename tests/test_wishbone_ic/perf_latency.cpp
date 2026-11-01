// 互连的主设备侧应答是组合多路选择：经互连访存的延迟必须和不接互连一样。
// 这条基准同时解释了为什么数据缓存对这个 CPU 没有收益——load 的 4 个周期是
// CPU 自己的流水线，总线延迟早就被藏在里面了。
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/platform/WishboneInterconnect.h>

using namespace dsc;

namespace {
struct Bus {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk, *rst, *irq, *mti, *msi, *halted;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    Net *s_adr, *s_dat_o, *s_dat_i, *s_we, *s_sel, *s_stb, *s_cyc, *s_ack;

    explicit Bus(bool with_ic) {
        auto cp = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = cp.get();
        circuit->addComponent(std::move(cp));
        WishboneInterconnect *ic = nullptr;
        if (with_ic) {
            auto icp = std::make_unique<WishboneInterconnect>("ic", "0:65536", 1);
            ic = icp.get();
            circuit->addComponent(std::move(icp));
        }
        auto mp = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mp.get();
        circuit->addComponent(std::move(mp));

        auto mk = [&](const char *n) { return circuit->createNet(n); };
        clk = mk("clk"); rst = mk("rst"); irq = mk("irq"); mti = mk("mti"); msi = mk("msi"); halted = mk("halted");
        wb_adr = mk("wb_adr"); wb_dat_o = mk("wb_dat_o"); wb_dat_i = mk("wb_dat_i");
        wb_we = mk("wb_we"); wb_sel = mk("wb_sel"); wb_stb = mk("wb_stb"); wb_cyc = mk("wb_cyc"); wb_ack = mk("wb_ack");
        s_adr = mk("s_adr"); s_dat_o = mk("s_dat_o"); s_dat_i = mk("s_dat_i");
        s_we = mk("s_we"); s_sel = mk("s_sel"); s_stb = mk("s_stb"); s_cyc = mk("s_cyc"); s_ack = mk("s_ack");

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

        if (with_ic) {
            circuit->connect(ic, "m0_adr_i", wb_adr);
            circuit->connect(ic, "m0_dat_i", wb_dat_o);
            circuit->connect(ic, "m0_we_i", wb_we);
            circuit->connect(ic, "m0_sel_i", wb_sel);
            circuit->connect(ic, "m0_stb_i", wb_stb);
            circuit->connect(ic, "m0_cyc_i", wb_cyc);
            circuit->connect(ic, "m0_dat_o", wb_dat_i);
            circuit->connect(ic, "m0_ack_o", wb_ack);
            circuit->connect(ic, "s0_adr_o", s_adr);
            circuit->connect(ic, "s0_dat_o", s_dat_o);
            circuit->connect(ic, "s0_we_o", s_we);
            circuit->connect(ic, "s0_sel_o", s_sel);
            circuit->connect(ic, "s0_stb_o", s_stb);
            circuit->connect(ic, "s0_cyc_o", s_cyc);
            circuit->connect(ic, "s0_dat_i", s_dat_i);
            circuit->connect(ic, "s0_ack_i", s_ack);
            circuit->connect(mem, "wb_adr_i", s_adr);
            circuit->connect(mem, "wb_dat_i", s_dat_o);
            circuit->connect(mem, "wb_we_i", s_we);
            circuit->connect(mem, "wb_sel_i", s_sel);
            circuit->connect(mem, "wb_stb_i", s_stb);
            circuit->connect(mem, "wb_cyc_i", s_cyc);
            circuit->connect(mem, "wb_dat_o", s_dat_i);
            circuit->connect(mem, "wb_ack_o", s_ack);
        } else {
            circuit->connect(mem, "wb_adr_i", wb_adr);
            circuit->connect(mem, "wb_dat_i", wb_dat_o);
            circuit->connect(mem, "wb_we_i", wb_we);
            circuit->connect(mem, "wb_sel_i", wb_sel);
            circuit->connect(mem, "wb_stb_i", wb_stb);
            circuit->connect(mem, "wb_cyc_i", wb_cyc);
            circuit->connect(mem, "wb_dat_o", wb_dat_i);
            circuit->connect(mem, "wb_ack_o", wb_ack);
        }
    }

    uint32_t loadAndSetup(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        size_t N = insts.size();
        uint32_t trap_addr = (uint32_t) (8 + N * 4);
        auto w = [&](size_t off, uint32_t inst) { std::memcpy(m + off, &inst, 4); };
        w(0, 0x00000093 | ((trap_addr & 0xFFF) << 20) | (1 << 7));
        w(4, 0x30509073);
        for (size_t i = 0; i < N; i++)
            w(8 + i * 4, insts[i]);
        w(8 + N * 4, 0x0000006F);
        return trap_addr;
    }

    // 跑到 PC 停在陷阱地址，返回用掉多少时钟周期
    uint64_t runToTrap(uint32_t trap, int max_cycles = 20000) {
        auto err = circuit->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        if (err.code != ErrorCode::OK)
            return 0;
        circuit->init();
        circuit->setWire(clk->id(), {0});
        circuit->setWire(rst->id(), {1});
        circuit->tick();
        circuit->setWire(rst->id(), {0});
        for (int i = 0; i < max_cycles; i++) {
            circuit->setWire(clk->id(), {0});
            circuit->tick();
            circuit->setWire(clk->id(), {1});
            circuit->tick();
            if (cpu->pc() == trap)
                return (uint64_t) i + 1;
        }
        return (uint64_t) max_cycles;
    }
};

// addi x1,0x100; sw; n 次 lw（同一地址）
std::vector<uint32_t> repeatedLoads(int n) {
    std::vector<uint32_t> p;
    p.push_back(0x00000093 | (0x100 << 20) | (1 << 7));
    p.push_back(0x0AA00293);
    p.push_back(0x0050A023);
    for (int i = 0; i < n; i++)
        p.push_back(0x0000A183);
    p.push_back(0x00100073);
    return p;
}
} // namespace

TEST(BusPerf, IcAddsNoAccessLatency) {
    // 用 N 与 N+1 次 load 的周期差量单次访存成本，去掉取指等固定开销
    auto marginal = [](bool with_ic, int n) {
        Bus a(with_ic), b(with_ic);
        uint64_t ca = a.runToTrap(a.loadAndSetup(repeatedLoads(n)));
        uint64_t cb = b.runToTrap(b.loadAndSetup(repeatedLoads(n + 1)));
        return (double) cb - (double) ca;
    };
    const double via_ic = marginal(true, 8);
    const double direct = marginal(false, 8);
    std::printf("单次 load：经互连 %.1f 周期，直连存储器 %.1f 周期\n", via_ic, direct);
    EXPECT_LE(via_ic, direct + 0.5) << "互连的主设备侧应答复用组合路径，不该比直连多一拍";
}
