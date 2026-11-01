// WishboneDcache 与 RISC-V CPU 的联调：真实取指/访存都经过缓存
#include <gtest/gtest.h>
#include <cstring>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/platform/WishboneDcache.h>

using namespace dsc;

namespace {
// CPU(0) → Dcache(1) → Memory(2)；监听端口直接接主端口，等价于互连把写事务
// 转发到 RAM 侧（自己的写直通不能把自己的行失效）
struct CpuCacheFixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU<32> *cpu = nullptr;
    WishboneDcache *cache = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk, *rst, *irq, *mti, *msi, *halted;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    Net *m_adr, *m_dat_o, *m_dat_i, *m_we, *m_sel, *m_stb, *m_cyc, *m_ack;

    CpuCacheFixture() {
        auto cp = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = cp.get();
        circuit->addComponent(std::move(cp));
        auto cc = std::make_unique<WishboneDcache>("dcache0", 16, 0, 0x10000);
        cache = cc.get();
        circuit->addComponent(std::move(cc));
        auto mp = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mp.get();
        circuit->addComponent(std::move(mp));

        auto mk = [&](const char *n) { return circuit->createNet(n); };
        clk = mk("clk"); rst = mk("rst"); irq = mk("irq"); mti = mk("mti"); msi = mk("msi"); halted = mk("halted");
        wb_adr = mk("wb_adr"); wb_dat_o = mk("wb_dat_o"); wb_dat_i = mk("wb_dat_i");
        wb_we = mk("wb_we"); wb_sel = mk("wb_sel"); wb_stb = mk("wb_stb"); wb_cyc = mk("wb_cyc");
        wb_ack = mk("wb_ack");
        m_adr = mk("m_adr"); m_dat_o = mk("m_dat_o"); m_dat_i = mk("m_dat_i");
        m_we = mk("m_we"); m_sel = mk("m_sel"); m_stb = mk("m_stb"); m_cyc = mk("m_cyc"); m_ack = mk("m_ack");

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

        circuit->connect(cache, "s_adr_i", wb_adr);
        circuit->connect(cache, "s_dat_i", wb_dat_o);
        circuit->connect(cache, "s_we_i", wb_we);
        circuit->connect(cache, "s_sel_i", wb_sel);
        circuit->connect(cache, "s_stb_i", wb_stb);
        circuit->connect(cache, "s_cyc_i", wb_cyc);
        circuit->connect(cache, "s_dat_o", wb_dat_i);
        circuit->connect(cache, "s_ack_o", wb_ack);

        circuit->connect(cache, "m_adr_o", m_adr);
        circuit->connect(cache, "m_dat_o", m_dat_o);
        circuit->connect(cache, "m_we_o", m_we);
        circuit->connect(cache, "m_sel_o", m_sel);
        circuit->connect(cache, "m_stb_o", m_stb);
        circuit->connect(cache, "m_cyc_o", m_cyc);
        circuit->connect(cache, "m_dat_i", m_dat_i);
        circuit->connect(cache, "m_ack_i", m_ack);

        circuit->connect(cache, "sn_adr_i", m_adr);
        circuit->connect(cache, "sn_we_i", m_we);
        circuit->connect(cache, "sn_stb_i", m_stb);
        circuit->connect(cache, "sn_cyc_i", m_cyc);

        circuit->connect(mem, "wb_adr_i", m_adr);
        circuit->connect(mem, "wb_dat_i", m_dat_o);
        circuit->connect(mem, "wb_we_i", m_we);
        circuit->connect(mem, "wb_sel_i", m_sel);
        circuit->connect(mem, "wb_stb_i", m_stb);
        circuit->connect(mem, "wb_cyc_i", m_cyc);
        circuit->connect(mem, "wb_dat_o", m_dat_i);
        circuit->connect(mem, "wb_ack_o", m_ack);
    }

    uint32_t loadAndSetup(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        size_t N = insts.size();
        uint32_t trap_addr = (uint32_t) (8 + N * 4);
        auto w = [&](size_t off, uint32_t inst) { std::memcpy(m + off, &inst, 4); };
        // addi x1, x0, trap_addr ; csrrw x0, mtvec, x1
        w(0, 0x00000093 | ((trap_addr & 0xFFF) << 20) | (1 << 7));
        w(4, 0x30509073);
        for (size_t i = 0; i < N; i++)
            w(8 + i * 4, insts[i]);
        w(8 + N * 4, 0x0000006F); // jal x0, 0
        return trap_addr;
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
    }

    void runCycles(int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), 0);
            circuit->tick();
            circuit->setWire(clk->id(), 1);
            circuit->tick();
        }
    }

    uint32_t peek(uint32_t addr) const {
        uint32_t v = 0;
        std::memcpy(&v, mem->writableMemPtr() + (addr & 0xFFFF), 4);
        return v;
    }
};
} // namespace

TEST(DcacheCpuTest, StoreThenLoadThroughCache) {
    CpuCacheFixture f;
    uint32_t trap = f.loadAndSetup({
            0x10000093, // addi x1, x0, 0x100
            0x12345137, // lui  x2, 0x12345
            0x67810113, // addi x2, x2, 0x678   → 0x12345678
            0x0020A023, // sw   x2, 0(x1)
            0x0000A183, // lw   x3, 0(x1)
            0x00100073, // ebreak
    });
    f.compile();
    f.circuit->setWire(f.clk->id(), 0);
    f.circuit->setWire(f.rst->id(), 1);
    f.circuit->tick();
    f.circuit->setWire(f.rst->id(), 0);
    f.runCycles(200);

    EXPECT_EQ(f.cpu->pc(), trap);
    EXPECT_EQ(f.cpu->reg(3), 0x12345678u) << "写直通后立刻读必须读到新值";
    EXPECT_EQ(f.peek(0x100), 0x12345678u);
}

TEST(DcacheCpuTest, RepeatedLoadsHitAndStayCorrect) {
    CpuCacheFixture f;
    f.loadAndSetup({
            0x10000093, // addi x1, x0, 0x100
            0x0AA00293, // addi x5, x0, 0xAA
            0x0050A023, // sw   x5, 0(x1)
            0x0000A183, // lw   x3, 0(x1)
            0x0000A203, // lw   x4, 0(x1)
            0x0040A283, // lw   x5, 4(x1)   → 0
            0x00100073,
    });
    f.compile();
    f.circuit->setWire(f.clk->id(), 0);
    f.circuit->setWire(f.rst->id(), 1);
    f.circuit->tick();
    f.circuit->setWire(f.rst->id(), 0);
    f.runCycles(200);

    EXPECT_EQ(f.cpu->reg(3), 0xAAu);
    EXPECT_EQ(f.cpu->reg(4), 0xAAu);
    EXPECT_EQ(f.cpu->reg(5), 0u) << "同一行的另一个字必须按字节取到各自的数";
}

TEST(DcacheCpuTest, ByteStoreAndLoad) {
    CpuCacheFixture f;
    f.loadAndSetup({
            0x10000093, // addi x1, x0, 0x100
            0x12345137, // lui  x2, 0x12345
            0x67810113, // addi x2, x2, 0x678
            0x0020A023, // sw   x2, 0(x1)      → 0x12345678
            0x05500293, // addi x5, x0, 0x55
            0x00508023, // sb   x5, 0(x1)      → 0x12345655
            0x0000A183, // lw   x3, 0(x1)
            0x0000A203, // lw   x4, 0(x1)
            0x00100073,
    });
    f.compile();
    f.circuit->setWire(f.clk->id(), 0);
    f.circuit->setWire(f.rst->id(), 1);
    f.circuit->tick();
    f.circuit->setWire(f.rst->id(), 0);
    f.runCycles(220);

    EXPECT_EQ(f.cpu->reg(3), 0x12345655u);
    EXPECT_EQ(f.cpu->reg(4), 0x12345655u);
    EXPECT_EQ(f.peek(0x100), 0x12345655u);
}
