// RISC-V CLINT 测试: 定时器中断 + 软件中断 + SFENCE.VMA + FENCE.I
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/WishboneInterconnect.h>
#include <dcs/components/platform/CLINT.h>
#include <cstring>

using namespace dsc;

// ============================================================
// SFENCE.VMA: 执行后不应 trap，正常到下一指令
// ============================================================
TEST(ClintTest, SFENCE_VMA) {
    auto circuit = std::make_unique<Circuit>();
    auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
    auto *cpu = cpu_ptr.get();
    circuit->addComponent(std::move(cpu_ptr));
    auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
    auto *mem = mem_ptr.get();
    circuit->addComponent(std::move(mem_ptr));

    auto clk = circuit->createNet("clk"); auto rst = circuit->createNet("rst");
    auto irq = circuit->createNet("irq"); auto mti = circuit->createNet("mti");
    auto msi = circuit->createNet("msi");
    auto adr = circuit->createNet("adr"); auto do_ = circuit->createNet("do");
    auto di = circuit->createNet("di"); auto we = circuit->createNet("we");
    auto sel = circuit->createNet("sel"); auto stb = circuit->createNet("stb");
    auto cyc = circuit->createNet("cyc"); auto ack = circuit->createNet("ack");
    auto halt = circuit->createNet("halt");

    circuit->connect(cpu, "clk", clk); circuit->connect(cpu, "rst", rst);
    circuit->connect(cpu, "irq", irq); circuit->connect(cpu, "mti", mti);
    circuit->connect(cpu, "msi", msi);
    circuit->connect(cpu, "wb_dat_i", di); circuit->connect(cpu, "wb_ack_i", ack);
    circuit->connect(cpu, "wb_adr_o", adr); circuit->connect(cpu, "wb_dat_o", do_);
    circuit->connect(cpu, "wb_we_o", we); circuit->connect(cpu, "wb_sel_o", sel);
    circuit->connect(cpu, "wb_stb_o", stb); circuit->connect(cpu, "wb_cyc_o", cyc);
    circuit->connect(cpu, "halted", halt);
    circuit->connect(mem, "clk", clk); circuit->connect(mem, "wb_adr_i", adr);
    circuit->connect(mem, "wb_dat_i", do_); circuit->connect(mem, "wb_we_i", we);
    circuit->connect(mem, "wb_sel_i", sel); circuit->connect(mem, "wb_stb_i", stb);
    circuit->connect(mem, "wb_cyc_i", cyc); circuit->connect(mem, "wb_dat_o", di);
    circuit->connect(mem, "wb_ack_o", ack);

    uint8_t *mp = mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    w32(0x00, 0x12000073); // sfence.vma x0,x0
    w32(0x04, 0x04D00193); // addi x3,x0,77
    w32(0x08, 0x0000006F); // jal x0,0

    auto err = circuit->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    circuit->init();

    auto run = [&](int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), {0}); circuit->tick();
            circuit->setWire(clk->id(), {1}); circuit->tick();
        }
    };
    circuit->setWire(clk->id(), {0}); circuit->setWire(rst->id(), {1});
    circuit->tick(); circuit->setWire(rst->id(), {0});
    run(40);

    EXPECT_EQ(cpu->reg(3), 77);
}

// ============================================================
// FENCE.I: 应作为合法 nop 执行
// ============================================================
TEST(ClintTest, FENCE_I) {
    auto circuit = std::make_unique<Circuit>();
    auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
    auto *cpu = cpu_ptr.get();
    circuit->addComponent(std::move(cpu_ptr));
    auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
    auto *mem = mem_ptr.get();
    circuit->addComponent(std::move(mem_ptr));

    auto clk = circuit->createNet("clk"); auto rst = circuit->createNet("rst");
    auto irq = circuit->createNet("irq"); auto mti = circuit->createNet("mti");
    auto msi = circuit->createNet("msi");
    auto adr = circuit->createNet("adr"); auto do_ = circuit->createNet("do");
    auto di = circuit->createNet("di"); auto we = circuit->createNet("we");
    auto sel = circuit->createNet("sel"); auto stb = circuit->createNet("stb");
    auto cyc = circuit->createNet("cyc"); auto ack = circuit->createNet("ack");
    auto halt = circuit->createNet("halt");

    circuit->connect(cpu, "clk", clk); circuit->connect(cpu, "rst", rst);
    circuit->connect(cpu, "irq", irq); circuit->connect(cpu, "mti", mti);
    circuit->connect(cpu, "msi", msi);
    circuit->connect(cpu, "wb_dat_i", di); circuit->connect(cpu, "wb_ack_i", ack);
    circuit->connect(cpu, "wb_adr_o", adr); circuit->connect(cpu, "wb_dat_o", do_);
    circuit->connect(cpu, "wb_we_o", we); circuit->connect(cpu, "wb_sel_o", sel);
    circuit->connect(cpu, "wb_stb_o", stb); circuit->connect(cpu, "wb_cyc_o", cyc);
    circuit->connect(cpu, "halted", halt);
    circuit->connect(mem, "clk", clk); circuit->connect(mem, "wb_adr_i", adr);
    circuit->connect(mem, "wb_dat_i", do_); circuit->connect(mem, "wb_we_i", we);
    circuit->connect(mem, "wb_sel_i", sel); circuit->connect(mem, "wb_stb_i", stb);
    circuit->connect(mem, "wb_cyc_i", cyc); circuit->connect(mem, "wb_dat_o", di);
    circuit->connect(mem, "wb_ack_o", ack);

    uint8_t *mp = mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };
    w32(0x00, 0x0000100F); // fence.i
    w32(0x04, 0x05800193); // addi x3,x0,88
    w32(0x08, 0x0000006F); // jal x0,0

    auto err = circuit->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    circuit->init();

    auto run = [&](int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), {0}); circuit->tick();
            circuit->setWire(clk->id(), {1}); circuit->tick();
        }
    };
    circuit->setWire(clk->id(), {0}); circuit->setWire(rst->id(), {1});
    circuit->tick(); circuit->setWire(rst->id(), {0});
    run(40);

    EXPECT_EQ(cpu->reg(3), 88);
}

// ============================================================
// 定时器中断: CPU + Memory + CLINT + WishboneInterconnect
// ============================================================
TEST(ClintTest, TimerInterrupt) {
    auto circuit = std::make_unique<Circuit>();

    // 元件
    auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
    auto *cpu = cpu_ptr.get();
    circuit->addComponent(std::move(cpu_ptr));
    auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 14, 32); // 16KB, addr 0-3FFF
    auto *mem = mem_ptr.get();
    circuit->addComponent(std::move(mem_ptr));
    auto clint_ptr = std::make_unique<CLINT>("clint0");
    auto *clint = clint_ptr.get();
    circuit->addComponent(std::move(clint_ptr));
    // Wishbone 互连: 0-16383→mem(s0), 16384-16639→clint(s1)
    // CLINT 偏移 0x4000=16384, 大小 256B (mtime 在 0xBFF8)
    // 使用更大的范围: 0x4000-0xC000 (包含所有 CLINT 寄存器)
    auto ic_ptr = std::make_unique<WishboneInterconnect>("ic0", "0:16384,16384:32768");
    auto *ic = ic_ptr.get();
    circuit->addComponent(std::move(ic_ptr));

    // 网络
    auto clk = circuit->createNet("clk"); auto rst = circuit->createNet("rst");
    auto irq = circuit->createNet("irq"); auto mti = circuit->createNet("mti");
    auto msi = circuit->createNet("msi");
    auto m_adr = circuit->createNet("m_adr"); auto m_do = circuit->createNet("m_do");
    auto m_di = circuit->createNet("m_di"); auto m_we = circuit->createNet("m_we");
    auto m_sel = circuit->createNet("m_sel"); auto m_stb = circuit->createNet("m_stb");
    auto m_cyc = circuit->createNet("m_cyc"); auto m_ack = circuit->createNet("m_ack");
    auto halt = circuit->createNet("halt");
    auto s0_do = circuit->createNet("s0_do"); auto s0_ack = circuit->createNet("s0_ack");
    auto s0_adr = circuit->createNet("s0_adr"); auto s0_wdat = circuit->createNet("s0_wdat");
    auto s0_we = circuit->createNet("s0_we"); auto s0_sel = circuit->createNet("s0_sel");
    auto s0_stb = circuit->createNet("s0_stb"); auto s0_cyc = circuit->createNet("s0_cyc");
    auto s1_do = circuit->createNet("s1_do"); auto s1_ack = circuit->createNet("s1_ack");
    auto s1_adr = circuit->createNet("s1_adr"); auto s1_wdat = circuit->createNet("s1_wdat");
    auto s1_we = circuit->createNet("s1_we"); auto s1_sel = circuit->createNet("s1_sel");
    auto s1_stb = circuit->createNet("s1_stb"); auto s1_cyc = circuit->createNet("s1_cyc");

    // CPU ↔ 互连主口
    circuit->connect(cpu, "clk", clk); circuit->connect(cpu, "rst", rst);
    circuit->connect(cpu, "irq", irq); circuit->connect(cpu, "mti", mti);
    circuit->connect(cpu, "msi", msi);
    circuit->connect(cpu, "wb_dat_i", m_di); circuit->connect(cpu, "wb_ack_i", m_ack);
    circuit->connect(cpu, "wb_adr_o", m_adr); circuit->connect(cpu, "wb_dat_o", m_do);
    circuit->connect(cpu, "wb_we_o", m_we); circuit->connect(cpu, "wb_sel_o", m_sel);
    circuit->connect(cpu, "wb_stb_o", m_stb); circuit->connect(cpu, "wb_cyc_o", m_cyc);
    circuit->connect(cpu, "halted", halt);

    // 互连主口 ↔ CPU (回传)
    circuit->connect(ic, "m0_adr_i", m_adr); circuit->connect(ic, "m0_dat_i", m_do);
    circuit->connect(ic, "m0_we_i", m_we); circuit->connect(ic, "m0_sel_i", m_sel);
    circuit->connect(ic, "m0_stb_i", m_stb); circuit->connect(ic, "m0_cyc_i", m_cyc);
    circuit->connect(ic, "s0_dat_i", s0_do); circuit->connect(ic, "s0_ack_i", s0_ack);
    circuit->connect(ic, "s1_dat_i", s1_do); circuit->connect(ic, "s1_ack_i", s1_ack);
    circuit->connect(ic, "m0_dat_o", m_di); circuit->connect(ic, "m0_ack_o", m_ack);
    circuit->connect(ic, "s0_adr_o", s0_adr); circuit->connect(ic, "s0_dat_o", s0_wdat);
    circuit->connect(ic, "s0_we_o", s0_we); circuit->connect(ic, "s0_sel_o", s0_sel);
    circuit->connect(ic, "s0_stb_o", s0_stb); circuit->connect(ic, "s0_cyc_o", s0_cyc);
    circuit->connect(ic, "s1_adr_o", s1_adr); circuit->connect(ic, "s1_dat_o", s1_wdat);
    circuit->connect(ic, "s1_we_o", s1_we); circuit->connect(ic, "s1_sel_o", s1_sel);
    circuit->connect(ic, "s1_stb_o", s1_stb); circuit->connect(ic, "s1_cyc_o", s1_cyc);

    // 互连从口 s0 → Memory
    circuit->connect(mem, "clk", clk);
    circuit->connect(mem, "wb_adr_i", s0_adr); circuit->connect(mem, "wb_dat_i", s0_wdat);
    circuit->connect(mem, "wb_we_i", s0_we); circuit->connect(mem, "wb_sel_i", s0_sel);
    circuit->connect(mem, "wb_stb_i", s0_stb); circuit->connect(mem, "wb_cyc_i", s0_cyc);
    circuit->connect(mem, "wb_dat_o", s0_do); circuit->connect(mem, "wb_ack_o", s0_ack);

    // 互连从口 s1 → CLINT
    circuit->connect(clint, "clk", clk);
    circuit->connect(clint, "wb_adr_i", s1_adr); circuit->connect(clint, "wb_dat_i", s1_wdat);
    circuit->connect(clint, "wb_we_i", s1_we); circuit->connect(clint, "wb_sel_i", s1_sel);
    circuit->connect(clint, "wb_stb_i", s1_stb); circuit->connect(clint, "wb_cyc_i", s1_cyc);
    circuit->connect(clint, "wb_dat_o", s1_do); circuit->connect(clint, "wb_ack_o", s1_ack);
    circuit->connect(clint, "mti", mti); circuit->connect(clint, "msi", msi);

    // 程序: 设置 mtvec + mie.MTIE + mstatus.MIE → SW 写 CLINT mtimecmp → spin 等待中断
    uint8_t *mp = mem->writableMemPtr();
    auto w32 = [&](size_t off, uint32_t v) {
        mp[off+0]=(uint8_t)(v&0xFF); mp[off+1]=(uint8_t)((v>>8)&0xFF);
        mp[off+2]=(uint8_t)((v>>16)&0xFF); mp[off+3]=(uint8_t)((v>>24)&0xFF);
    };

    w32(0x00, 0x04000093); // addi x1,x0,0x40
    w32(0x04, 0x30509073); // csrrw x0,mtvec,x1        (mtvec=0x40)
    w32(0x08, 0x08000093); // addi x1,x0,0x80
    w32(0x0C, 0x30409073); // csrrw x0,mie,x1          (mie.MTIE=1)
    w32(0x10, 0x00800093); // addi x1,x0,8
    w32(0x14, 0x30009073); // csrrw x0,mstatus,x1      (mstatus.MIE=1)
    // 用 lui 设置地址 0x4000，然后 sw 写 mtimecmp
    w32(0x18, 0x000042B7); // lui x5, 0x4              (x5=0x4000)
    w32(0x1C, 0x0002A023); // sw x0, 0(x5)            (mtimecmp[31:0]=0)
    // 高半也要显式写 0：复位值现在是"永不触发"(全 1)，只清低半仍不会到期
    w32(0x20, 0x0002A223); // sw x0, 4(x5)            (mtimecmp[63:32]=0)
    w32(0x24, 0x0000006F); // jal x0,0 (spin)
    // 0x40: 中断处理程序
    w32(0x40, 0x06300193); // addi x3,x0,99
    w32(0x44, 0x30200073); // mret

    auto err = circuit->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    circuit->init();

    auto run = [&](int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), {0}); circuit->tick();
            circuit->setWire(clk->id(), {1}); circuit->tick();
        }
    };
    circuit->setWire(clk->id(), {0}); circuit->setWire(rst->id(), {1});
    circuit->setWire(irq->id(), {0});
    circuit->tick(); circuit->setWire(rst->id(), {0});
    run(300);

    // 写 mtimecmp=0 后 mtime 已 >= mtimecmp，MTI 触发中断
    // 处理程序设 x3=99
    EXPECT_EQ(cpu->reg(3), 99);
}

// ============================================================
// 复位后 mti 必须保持低电平
// ============================================================
// mtimecmp 若复位成 0，mtime >= mtimecmp 从第一拍就恒真，mti 永远挂着。
// 真实平台有 M 态固件（OpenSBI）初始化它，本核没有；留 0 会让内核一打开
// 定时器中断就陷进中断风暴，启动日志停住不动
TEST(ClintTest, MtiLowAfterReset) {
    auto circuit = std::make_unique<Circuit>();
    auto cp = std::make_unique<CLINT>("clint0");
    auto *clint = cp.get();
    circuit->addComponent(std::move(cp));

    auto clk = circuit->createNet("clk");
    auto mti = circuit->createNet("mti");
    auto msi = circuit->createNet("msi");
    auto adr = circuit->createNet("adr");
    auto wdat = circuit->createNet("wdat");
    auto we = circuit->createNet("we");
    auto sel = circuit->createNet("sel");
    auto stb = circuit->createNet("stb");
    auto cyc = circuit->createNet("cyc");
    auto dout = circuit->createNet("dout");
    auto ack = circuit->createNet("ack");
    circuit->connect(clint, "clk", clk);
    circuit->connect(clint, "wb_adr_i", adr);
    circuit->connect(clint, "wb_dat_i", wdat);
    circuit->connect(clint, "wb_we_i", we);
    circuit->connect(clint, "wb_sel_i", sel);
    circuit->connect(clint, "wb_stb_i", stb);
    circuit->connect(clint, "wb_cyc_i", cyc);
    circuit->connect(clint, "wb_dat_o", dout);
    circuit->connect(clint, "wb_ack_o", ack);
    circuit->connect(clint, "mti", mti);
    circuit->connect(clint, "msi", msi);

    auto err = circuit->compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    circuit->init();
    for (int i = 0; i < 1000; i++) {
        circuit->setWire(clk->id(), {0});
        circuit->tick();
        circuit->setWire(clk->id(), {1});
        circuit->tick();
    }
    EXPECT_EQ(circuit->getWire(mti->id())[0], 0)
        << "mtimecmp 复位值必须是永不触发，为 0 会让 mti 从第一拍起恒高";
}

// ============================================================
// CLINT 寄存器：mtime 推进、mtimecmp 比较、msip 软件中断
// ============================================================
namespace {
struct ClintRegFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    CLINT *clint = nullptr;
    Net *clk, *adr, *dat_i, *dat_o, *we, *sel, *stb, *cyc, *ack, *mti, *msi;

    ClintRegFixture() {
        auto p = std::make_unique<CLINT>("clint0");
        clint = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat_i = c->createNet("di");
        dat_o = c->createNet("do"); we = c->createNet("we"); sel = c->createNet("sel");
        stb = c->createNet("stb"); cyc = c->createNet("cyc"); ack = c->createNet("ack");
        mti = c->createNet("mti"); msi = c->createNet("msi");
        for (auto [pin, net]: {std::pair{"clk", clk}, {"wb_adr_i", adr}, {"wb_dat_i", dat_i},
                               {"wb_we_i", we}, {"wb_sel_i", sel}, {"wb_stb_i", stb}, {"wb_cyc_i", cyc},
                               {"wb_dat_o", dat_o}, {"wb_ack_o", ack}, {"mti", mti}, {"msi", msi}})
            c->connect(clint, pin, net);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
        c->setWire(clk->id(), {0});
        c->setWire(stb->id(), {0});
        c->setWire(cyc->id(), {0});
        c->setWire(we->id(), {0});
        for (int i = 0; i < 2; i++) tick();
    }
    void tick() {
        c->setWire(clk->id(), {0}); c->tick();
        c->setWire(clk->id(), {1}); c->tick();
    }
    void set32(Net *n, uint32_t v) {
        c->setWire(n->id(), {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)});
    }
    uint32_t get32(Net *n) {
        auto v = c->getWire(n->id());
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    void write(uint32_t a, uint32_t v) {
        set32(adr, a); set32(dat_i, v);
        c->setWire(we->id(), {1}); c->setWire(sel->id(), {0x0F});
        c->setWire(stb->id(), {1}); c->setWire(cyc->id(), {1});
        for (int i = 0; i < 4; i++) tick();
        c->setWire(stb->id(), {0}); c->setWire(cyc->id(), {0});
        for (int i = 0; i < 2; i++) tick();
    }
    void write64(uint32_t a, uint64_t v) {
        write(a, (uint32_t) v);
        write(a + 4, (uint32_t) (v >> 32));
    }
    uint32_t read(uint32_t a) {
        set32(adr, a);
        c->setWire(we->id(), {0}); c->setWire(sel->id(), {0x0F});
        c->setWire(stb->id(), {1}); c->setWire(cyc->id(), {1});
        for (int i = 0; i < 6; i++) tick();
        auto v = get32(dat_o);
        c->setWire(stb->id(), {0}); c->setWire(cyc->id(), {0});
        for (int i = 0; i < 2; i++) tick();
        return v;
    }
};
} // namespace

TEST(ClintRegisterTest, MtimeAdvancesEveryTick) {
    ClintRegFixture f;
    uint32_t t0 = f.read(0xBFF8);
    for (int i = 0; i < 20; i++) f.tick();
    uint32_t t1 = f.read(0xBFF8);
    EXPECT_GT(t1, t0) << "mtime 应每拍递增";
}

TEST(ClintRegisterTest, MtimecmpInFutureKeepsMtiLow) {
    ClintRegFixture f;
    f.write64(0x4000, 0xFFFFFFF0ULL);
    for (int i = 0; i < 4; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.mti->id())[0], 0) << "mtime < mtimecmp 时不应拉 mti";
}

TEST(ClintRegisterTest, MtimecmpInPastRaisesMti) {
    ClintRegFixture f;
    f.write64(0x4000, 0);
    for (int i = 0; i < 4; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.mti->id())[0], 1) << "mtime >= mtimecmp 时应拉 mti";
    f.write64(0x4000, 0xFFFFFFF0ULL);
    for (int i = 0; i < 4; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.mti->id())[0], 0) << "重新设到未来后 mti 应撤销";
}

TEST(ClintRegisterTest, MsipDrivesMsiAndClears) {
    ClintRegFixture f;
    f.write(0x0000, 1);
    for (int i = 0; i < 2; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.msi->id())[0], 1) << "msip=1 应拉 msi";
    f.write(0x0000, 0);
    for (int i = 0; i < 2; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.msi->id())[0], 0) << "msip=0 应撤销 msi";
}

TEST(ClintRegisterTest, UnknownOffsetReadsZero) {
    ClintRegFixture f;
    EXPECT_EQ(f.read(0x2000), 0u) << "未实现的 CLINT 偏移应读 0";
}

