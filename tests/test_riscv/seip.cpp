// S 级外部中断：seip 引脚应反映到 mip.SEIP，并能委托到 S 模式
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>

using namespace dsc;

namespace {
struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    RiscVCPU *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk, *rst, *irq, *mti, *msi, *seip;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack, *halted;

    Fx() {
        auto p = std::make_unique<RiscVCPU>("cpu");
        cpu = p.get();
        c->addComponent(std::move(p));
        auto m = std::make_unique<WishboneMemory>("mem", 16, 32);
        mem = m.get();
        c->addComponent(std::move(m));
        clk = c->createNet("clk"); rst = c->createNet("rst"); irq = c->createNet("irq");
        mti = c->createNet("mti"); msi = c->createNet("msi"); seip = c->createNet("seip");
        wb_adr = c->createNet("wa"); wb_dat_o = c->createNet("wdo"); wb_dat_i = c->createNet("wdi");
        wb_we = c->createNet("wwe"); wb_sel = c->createNet("wsel"); wb_stb = c->createNet("wstb");
        wb_cyc = c->createNet("wcyc"); wb_ack = c->createNet("wak"); halted = c->createNet("halt");
        c->connect(cpu, "clk", clk); c->connect(cpu, "rst", rst);
        c->connect(cpu, "irq", irq); c->connect(cpu, "mti", mti);
        c->connect(cpu, "msi", msi); c->connect(cpu, "seip", seip);
        c->connect(cpu, "wb_dat_i", wb_dat_i); c->connect(cpu, "wb_ack_i", wb_ack);
        c->connect(cpu, "wb_adr_o", wb_adr); c->connect(cpu, "wb_dat_o", wb_dat_o);
        c->connect(cpu, "wb_we_o", wb_we); c->connect(cpu, "wb_sel_o", wb_sel);
        c->connect(cpu, "wb_stb_o", wb_stb); c->connect(cpu, "wb_cyc_o", wb_cyc);
        c->connect(cpu, "halted", halted);
        c->connect(mem, "clk", clk); c->connect(mem, "wb_adr_i", wb_adr);
        c->connect(mem, "wb_dat_i", wb_dat_o); c->connect(mem, "wb_we_i", wb_we);
        c->connect(mem, "wb_sel_i", wb_sel); c->connect(mem, "wb_stb_i", wb_stb);
        c->connect(mem, "wb_cyc_i", wb_cyc); c->connect(mem, "wb_dat_o", wb_dat_i);
        c->connect(mem, "wb_ack_o", wb_ack);
    }
    void load(const std::vector<uint32_t> &v) {
        uint8_t *m = mem->writableMemPtr();
        for (size_t i = 0; i < v.size(); i++)
            for (int b = 0; b < 4; b++)
                m[i * 4 + b] = (uint8_t)((v[i] >> (8 * b)) & 0xFF);
    }
    void run(int n, bool seip_high) {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        c->setWire(clk->id(), {(uint8_t) 0});
        c->setWire(rst->id(), {(uint8_t) 1});
        c->setWire(seip->id(), {(uint8_t) (seip_high ? 1 : 0)});
        c->tick();
        c->setWire(rst->id(), {(uint8_t) 0});
        for (int i = 0; i < n; ++i) {
            c->setWire(clk->id(), {(uint8_t) 0}); c->tick();
            c->setWire(clk->id(), {(uint8_t) 1}); c->tick();
        }
    }
};
} // namespace

TEST(SeipTest, ReflectsIntoMip) {
    Fx f;
    // csrrs x7, mip, x0  然后 ebreak
    f.load({0x344023F3u, 0x00100073u});
    f.run(30, true);
    EXPECT_NE(f.cpu->reg(7) & (1u << 9), 0u) << "seip 拉高后 mip.SEIP（bit9）应为 1";
}

TEST(SeipTest, ZeroWhenPinLow) {
    Fx f;
    f.load({0x344023F3u, 0x00100073u});
    f.run(30, false);
    EXPECT_EQ(f.cpu->reg(7) & (1u << 9), 0u) << "seip 为低时 mip.SEIP 应为 0";
}
