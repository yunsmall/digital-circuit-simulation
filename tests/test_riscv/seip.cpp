// S 级外部中断：seip 引脚应反映到 mip.SEIP，并能委托到 S 模式
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include "common/wb_cpu_link.h"
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

namespace {
struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk, *rst, *irq, *mti, *msi, *seip;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack, *halted;

    Fx() {
        auto p = std::make_unique<RiscVCPU<32>>("cpu");
        cpu = p.get();
        c->addComponent(std::move(p));
        auto link = test::linkCpuToWb(*c, cpu);
        auto m = std::make_unique<WishboneMemory>("mem", 16, 32);
        mem = m.get();
        c->addComponent(std::move(m));
        clk = c->createNet("clk"); rst = c->createNet("rst"); irq = c->createNet("irq");
        mti = c->createNet("mti"); msi = c->createNet("msi"); seip = c->createNet("seip");
        wb_adr = link.adr; wb_dat_o = link.dat_o; wb_dat_i = link.dat_i;
        wb_we = link.we; wb_sel = link.sel; wb_stb = link.stb;
        wb_cyc = link.cyc; wb_ack = link.ack; halted = c->createNet("halt");
        c->connect(cpu, "clk", clk); c->connect(cpu, "rst", rst);
        c->connect(cpu, "irq", irq); c->connect(cpu, "mti", mti);
        c->connect(cpu, "msi", msi); c->connect(cpu, "seip", seip);




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
        c->setWire(clk->id(), (uint8_t) 0);
        c->setWire(rst->id(), (uint8_t) 1);
        c->setWire(seip->id(), (uint8_t) (seip_high ? 1 : 0));
        c->tick();
        c->setWire(rst->id(), (uint8_t) 0);
        for (int i = 0; i < n; ++i) {
            c->setWire(clk->id(), (uint8_t) 0); c->tick();
            c->setWire(clk->id(), (uint8_t) 1); c->tick();
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

// 规范 3.1.9：mip.SEIP 是软件可写的，M 态写它向 S 态投递外部中断。
// 写进去的位必须能读回，不能下一拍被外部 seip 引脚覆盖
TEST(SeipTest, SoftwareWritableSeip) {
    Fx f;
    f.load({
        0x20000093u,   // addi x1, x0, 0x200  (mip.SEIP)
        0x34409073u,   // csrrw x0, mip, x1
        0x344023F3u,   // csrrs x7, mip, x0
        0x0000006Fu,
    });
    f.run(30, false);
    EXPECT_NE(f.cpu->reg(7) & (1u << 9), 0u) << "软件写入的 SEIP 应能读回";
}

// 写 mip=0 只能清软件位，本拍外部引脚给出的 SEIP 必须保留
TEST(SeipTest, ClearingMipKeepsExternalSeip) {
    Fx f;
    f.load({
        0x00000093u,   // addi x1, x0, 0
        0x34409073u,   // csrrw x0, mip, x1
        0x344023F3u,   // csrrs x7, mip, x0
        0x0000006Fu,
    });
    f.run(30, true);
    EXPECT_NE(f.cpu->reg(7) & (1u << 9), 0u) << "外部 SEIP 不能被软件写 0 清掉";
}

// 没有 Sstc 时 STIP 也是软件可写的
TEST(SeipTest, SoftwareWritableStip) {
    Fx f;
    f.load({
        0x02000093u,   // addi x1, x0, 0x20  (mip.STIP)
        0x34409073u,   // csrrw x0, mip, x1
        0x344023F3u,   // csrrs x7, mip, x0
        0x0000006Fu,
    });
    f.run(30, false);
    EXPECT_NE(f.cpu->reg(7) & (1u << 5), 0u) << "软件写入的 STIP 应能读回";
}
