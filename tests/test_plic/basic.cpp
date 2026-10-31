// PLIC 基本行为：优先级、使能、阈值、claim/complete、irq 输出
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/platform/PLIC.h>
#include <dcs/components/io/UART.h>

using namespace dsc;

namespace {

struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    PLIC *plic = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *dat_o = nullptr;
    Net *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
    Net *src = nullptr, *ack = nullptr, *irq = nullptr;

    explicit Fixture(int sources = 4) {
        auto p = std::make_unique<PLIC>("plic0", sources);
        plic = p.get();
        circuit->addComponent(std::move(p));
        clk = circuit->createNet("clk");
        adr = circuit->createNet("adr");
        dat_i = circuit->createNet("dat_i");
        dat_o = circuit->createNet("dat_o");
        we = circuit->createNet("we");
        sel = circuit->createNet("sel");
        stb = circuit->createNet("stb");
        cyc = circuit->createNet("cyc");
        src = circuit->createNet("src");
        ack = circuit->createNet("ack");
        irq = circuit->createNet("irq");
        circuit->connect(plic, "clk", clk);
        circuit->connect(plic, "wb_adr_i", adr);
        circuit->connect(plic, "wb_dat_i", dat_i);
        circuit->connect(plic, "wb_we_i", we);
        circuit->connect(plic, "wb_sel_i", sel);
        circuit->connect(plic, "wb_stb_i", stb);
        circuit->connect(plic, "wb_cyc_i", cyc);
        circuit->connect(plic, "irq_src", src);
        circuit->connect(plic, "wb_dat_o", dat_o);
        circuit->connect(plic, "wb_ack_o", ack);
        circuit->connect(plic, "irq", irq);
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), {0});
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        circuit->setWire(we->id(), {0});
    }

    void cycle() {
        circuit->setWire(clk->id(), {0});
        circuit->tick();
        circuit->setWire(clk->id(), {1});
        circuit->tick();
    }

    void write(uint32_t addr, uint32_t val) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
                                    (uint8_t)((addr >> 24) & 0xFF)});
        circuit->setWire(dat_i->id(), {(uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF), (uint8_t)((val >> 16) & 0xFF),
                                      (uint8_t)((val >> 24) & 0xFF)});
        circuit->setWire(we->id(), {1});
        circuit->setWire(sel->id(), {0x0F});
        circuit->setWire(stb->id(), {1});
        circuit->setWire(cyc->id(), {1});
        for (int i = 0; i < 6; ++i)
            cycle();
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        for (int i = 0; i < 4; ++i)
            cycle();
    }

    uint32_t read(uint32_t addr) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
                                    (uint8_t)((addr >> 24) & 0xFF)});
        circuit->setWire(we->id(), {0});
        circuit->setWire(sel->id(), {0x0F});
        circuit->setWire(stb->id(), {1});
        circuit->setWire(cyc->id(), {1});
        for (int i = 0; i < 8; ++i)
            cycle();
        auto v = circuit->getWire(dat_o->id());
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        for (int i = 0; i < 4; ++i)
            cycle();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }

    void setSrc(uint32_t mask) {
        circuit->setWire(src->id(), {(uint8_t)(mask & 0xFF)});
        cycle();
        cycle(); // 时序输出到线网要再等一拍
    }
    bool irqHigh() { return circuit->getWire(irq->id())[0] != 0; }
};

} // namespace

TEST(PlicTest, PriorityAndEnableGate) {
    Fixture f;
    f.compile();
    f.write(0x4, 3);      // source 1 priority = 3
    f.write(0x2000, 1u << 1); // enable source 1
    EXPECT_FALSE(f.irqHigh());
    f.setSrc(0x01);        // source 1 到达
    EXPECT_TRUE(f.irqHigh()) << "使能且优先级高于阈值时应拉高中断";
    f.setSrc(0x00);
}

TEST(PlicTest, ThresholdBlocksInterrupt) {
    Fixture f;
    f.compile();
    f.write(0x4, 2);
    f.write(0x2000, 1u << 1);
    f.write(0x200000, 2); // 阈值 = 2，优先级 2 不高于阈值
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh());
}

TEST(PlicTest, ClaimAndComplete) {
    Fixture f;
    f.compile();
    f.write(0x4, 5);       // source 1 高优先级
    f.write(0x8, 3);       // source 2 低优先级
    f.write(0x2000, (1u << 1) | (1u << 2));
    f.setSrc(0x03);
    EXPECT_TRUE(f.irqHigh());
    EXPECT_EQ(f.read(0x200004), 1u) << "claim 应返回优先级最高的 source 1";
    f.setSrc(0x03);
    EXPECT_EQ(f.read(0x200004), 2u) << "source 1 在服务中，claim 应返回 source 2";
    f.write(0x200004, 1);  // complete source 1
    f.setSrc(0x01);
    EXPECT_EQ(f.read(0x200004), 1u) << "complete 后 source 1 可再次触发";
}

TEST(PlicTest, SamePriorityTakesLowestSource) {
    Fixture f;
    f.compile();
    f.write(0x4, 4); // source 1
    f.write(0x8, 4); // source 2 同优先级
    f.write(0x2000, (1u << 1) | (1u << 2));
    f.setSrc(0x03);
    EXPECT_EQ(f.read(0x200004), 1u) << "同优先级应取源号小的";
    EXPECT_EQ(f.read(0x200004), 2u) << "claim 掉源 1 后应轮到源 2";
}

TEST(PlicTest, DisabledSourceNeverRaisesIrq) {
    Fixture f;
    f.compile();
    f.write(0x4, 7); // 高优先级但不使能
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh()) << "未使能的源不应拉高中断";
    EXPECT_EQ(f.read(0x200004), 0u) << "未使能的源不应被 claim";
}

TEST(PlicTest, PriorityEqualThresholdDoesNotTrigger) {
    Fixture f;
    f.compile();
    f.write(0x4, 3);
    f.write(0x2000, 1u << 1);
    f.write(0x200000, 3); // 阈值等于优先级，标准要求严格大于才触发
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh()) << "优先级不高于阈值时不该拉高中断";
    // 规范明确 claim 不受 threshold 影响，所以这里仍然能取走源 1
    EXPECT_EQ(f.read(0x200004), 1u) << "claim 不受 threshold 影响";
}

TEST(PlicTest, ClaimClearsPendingAndSetsInService) {
    Fixture f;
    f.compile();
    f.write(0x4, 5);
    f.write(0x2000, 1u << 1);
    f.setSrc(0x01);
    EXPECT_EQ(f.read(0x1000) & (1u << 1), 1u << 1) << "到达后 pending 位应为 1";
    EXPECT_EQ(f.read(0x200004), 1u);
    EXPECT_EQ(f.read(0x1000) & (1u << 1), 0u) << "claim 后 pending 位应清掉";
    f.setSrc(0x00);
    f.setSrc(0x01); // 重新拉高也不该再次进入 pending
    EXPECT_EQ(f.read(0x1000) & (1u << 1), 0u) << "在服务中的源不应重新挂起";
    f.write(0x200004, 1); // complete
    f.setSrc(0x00);
    f.setSrc(0x01);
    EXPECT_EQ(f.read(0x1000) & (1u << 1), 1u << 1) << "complete 后源应能重新挂起";
}

TEST(PlicTest, MultipleSourcesArriveTogether) {
    Fixture f;
    f.compile();
    f.write(0x4, 2);
    f.write(0x8, 6);
    f.write(0xC, 4);
    f.write(0x2000, (1u << 1) | (1u << 2) | (1u << 3));
    f.setSrc(0x07); // 三个源同一拍到达
    const uint32_t pend = f.read(0x1000);
    EXPECT_EQ(pend & (1u << 1), 1u << 1);
    EXPECT_EQ(pend & (1u << 2), 1u << 2);
    EXPECT_EQ(pend & (1u << 3), 1u << 3);
    EXPECT_EQ(f.read(0x200004), 2u) << "应先返回优先级最高的源 2";
}

TEST(PlicTest, UnmappedOffsetReadsZeroAndStillAcks) {
    Fixture f;
    f.compile();
    f.write(0x300000, 0xFFFFFFFF); // 未实现的偏移，写被忽略
    f.circuit->setWire(f.adr->id(), {0x00, 0x00, 0x30, 0x00});
    f.circuit->setWire(f.we->id(), {0});
    f.circuit->setWire(f.stb->id(), {1});
    f.circuit->setWire(f.cyc->id(), {1});
    for (int i = 0; i < 4; ++i)
        f.cycle();
    EXPECT_NE(f.circuit->getWire(f.ack->id())[0], 0) << "未实现的偏移也必须应答，否则总线挂死";
    auto v = f.circuit->getWire(f.dat_o->id());
    EXPECT_EQ(v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24), 0u);
}

TEST(PlicTest, PriorityRegisterMaskedToThreeBits) {
    Fixture f;
    f.compile();
    f.write(0x4, 0xFF); // PLIC 优先级只有 3 位
    EXPECT_EQ(f.read(0x4), 7u);
}

TEST(PlicTest, ThresholdRegisterMaskedToThreeBits) {
    Fixture f;
    f.compile();
    f.write(0x200000, 0xFF);
    EXPECT_EQ(f.read(0x200000), 7u);
}

// 内核在 S 态用的是 context 1：enable 基址 0x2080、threshold 0x201000、claim 0x201004。
// 这组偏移写错的话，内核写的使能会被当成"未实现偏移"丢掉，外部中断永远送不进去。
TEST(PlicTest, SContextHasSeparateRegisters) {
    Fixture f;
    f.compile();
    f.write(0x4, 3);        // 源 1 优先级
    f.write(0x2080, 1u << 1); // context 1 使能源 1
    f.setSrc(0x01);
    EXPECT_TRUE(f.irqHigh()) << "S context 使能后应拉高中断";
    EXPECT_EQ(f.read(0x201004), 1u) << "S context 的 claim 应返回源 1";
    // context 0 没有使能，不应被它 claim 走
    EXPECT_EQ(f.read(0x200004), 0u);
}

TEST(PlicTest, ContextsHaveIndependentThresholds) {
    Fixture f;
    f.compile();
    f.write(0x4, 3);
    f.write(0x2080, 1u << 1);   // 只在 context 1 使能
    f.write(0x200000, 7);     // context 0 阈值高
    f.write(0x201000, 0);     // context 1 阈值 0
    f.setSrc(0x01);
    EXPECT_TRUE(f.irqHigh()) << "context 1 阈值满足即应中断";
    f.write(0x201000, 3);     // context 1 阈值等于优先级，严格大于才触发
    f.setSrc(0x00);
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh());
}

// 规范把 priority=0 定义为 "never interrupt"。复位值就是 0，各源优先级由
// 内核在使能中断时自己写 1，所以默认状态必须是"不触发"。
TEST(PlicTest, ZeroPriorityNeverInterrupts) {
    Fixture f;
    f.compile();
    f.write(0x2000, 1u << 1); // 只使能，不碰优先级寄存器
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh()) << "priority=0 表示 never interrupt";
    EXPECT_EQ(f.read(0x200004), 0u);
}

// 真实场景：UART 的中断线直接接到 PLIC 的 irq_src，模拟内核对两者的寄存器操作。
// 只测 PLIC 自身的契约发现不了"外设到中断控制器"这一层的接线/语义问题。
TEST(PlicTest, UartIrqReachesPlicAndClaimWorks) {
    Circuit c;
    auto u = std::make_unique<UART>("uart0", 8);
    auto p = std::make_unique<PLIC>("plic0", 8);
    UART *uart = u.get();
    PLIC *plic = p.get();
    c.addComponent(std::move(u));
    c.addComponent(std::move(p));

    auto clk = c.createNet("clk");
    auto uart_irq = c.createNet("uart_irq");
    // UART 的总线
    auto ua = c.createNet("ua"), udin = c.createNet("udin"), udout = c.createNet("udout");
    auto uwe = c.createNet("uwe"), usel = c.createNet("usel"), ustb = c.createNet("ustb"), ucyc = c.createNet("ucyc");
    auto uack = c.createNet("uack"), utx = c.createNet("utx"), urx = c.createNet("urx");
    // PLIC 的总线
    auto pa = c.createNet("pa"), pdin = c.createNet("pdin"), pdout = c.createNet("pdout");
    auto pwe = c.createNet("pwe"), psel = c.createNet("psel"), pstb = c.createNet("pstb"), pcyc = c.createNet("pcyc");
    auto pack = c.createNet("pack"), pirq = c.createNet("pirq");

    c.connect(uart, "clk", clk);
    c.connect(uart, "wb_adr_i", ua);
    c.connect(uart, "wb_dat_i", udin);
    c.connect(uart, "wb_we_i", uwe);
    c.connect(uart, "wb_sel_i", usel);
    c.connect(uart, "wb_stb_i", ustb);
    c.connect(uart, "wb_cyc_i", ucyc);
    c.connect(uart, "rx", urx);
    c.connect(uart, "wb_dat_o", udout);
    c.connect(uart, "wb_ack_o", uack);
    c.connect(uart, "tx", utx);
    c.connect(uart, "irq", uart_irq);

    c.connect(plic, "clk", clk);
    c.connect(plic, "wb_adr_i", pa);
    c.connect(plic, "wb_dat_i", pdin);
    c.connect(plic, "wb_we_i", pwe);
    c.connect(plic, "wb_sel_i", psel);
    c.connect(plic, "wb_stb_i", pstb);
    c.connect(plic, "wb_cyc_i", pcyc);
    c.connect(plic, "irq_src", uart_irq);
    c.connect(plic, "wb_dat_o", pdout);
    c.connect(plic, "wb_ack_o", pack);
    c.connect(plic, "irq", pirq);

    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    c.init();
    for (auto *n : {clk, uwe, ustb, ucyc, pwe, pstb, pcyc})
        c.setWire(n->id(), {(uint8_t) 0});
    c.setWire(usel->id(), {(uint8_t) 0x0F});
    c.setWire(psel->id(), {(uint8_t) 0x0F});
    c.setWire(urx->id(), {(uint8_t) 1}); // 串口线空闲为高

    auto cycle = [&] {
        c.setWire(clk->id(), {0});
        c.tick();
        c.setWire(clk->id(), {1});
        c.tick();
    };
    for (int i = 0; i < 2; ++i)
        cycle();

    auto writeReg = [&](Net *adr, Net *dat, Net *we, Net *stb, Net *cyc, uint32_t a, uint32_t v) {
        c.setWire(adr->id(), {(uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF), (uint8_t)((a >> 16) & 0xFF),
                              (uint8_t)((a >> 24) & 0xFF)});
        c.setWire(dat->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
                              (uint8_t)((v >> 24) & 0xFF)});
        c.setWire(we->id(), {(uint8_t) 1});
        c.setWire(stb->id(), {(uint8_t) 1});
        c.setWire(cyc->id(), {(uint8_t) 1});
        for (int i = 0; i < 4; ++i)
            cycle();
        c.setWire(stb->id(), {(uint8_t) 0});
        c.setWire(cyc->id(), {(uint8_t) 0});
        for (int i = 0; i < 2; ++i)
            cycle();
    };

    // 内核对 UART：LCR=0x03（DLAB=0），IER=0x02（使能 THRE 中断）
    writeReg(ua, udin, uwe, ustb, ucyc, 0x0C, 0x03);
    writeReg(ua, udin, uwe, ustb, ucyc, 0x04, 0x02);
    // 内核对 PLIC（S 态 context 1，enable 基址 0x2080）：threshold=0，使能源 1，优先级 1
    writeReg(pa, pdin, pwe, pstb, pcyc, 0x201000, 0);
    writeReg(pa, pdin, pwe, pstb, pcyc, 0x2080, 2);
    writeReg(pa, pdin, pwe, pstb, pcyc, 0x4, 1);

    EXPECT_NE(c.getWire(pirq->id())[0], 0) << "UART 的 THRE 中断应经 PLIC 送到 irq 输出";

    // claim 应返回源 1
    c.setWire(pa->id(), {0x04, 0x10, 0x20, 0x00}); // 0x201004
    c.setWire(pwe->id(), {(uint8_t) 0});
    c.setWire(pstb->id(), {(uint8_t) 1});
    c.setWire(pcyc->id(), {(uint8_t) 1});
    for (int i = 0; i < 4; ++i)
        cycle();
    auto v = c.getWire(pdout->id());
    const uint32_t claim = uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    EXPECT_EQ(claim, 1u) << "claim 应返回 UART 对应的源 1";
    c.setWire(pstb->id(), {(uint8_t) 0});
    c.setWire(pcyc->id(), {(uint8_t) 0});
    for (int i = 0; i < 2; ++i)
        cycle();
}
