// UART 基本行为：发送位序列、接收采样、状态寄存器
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/UART.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    UART *uart = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *dat_o = nullptr;
    Net *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
    Net *rx = nullptr, *ack = nullptr, *tx = nullptr, *irq = nullptr;

    explicit Fixture(int baud = 2, bool loopback = false) {
        auto u = std::make_unique<UART>("uart0", baud);
        uart = u.get();
        circuit->addComponent(std::move(u));
        clk = circuit->createNet("clk");
        adr = circuit->createNet("adr");
        dat_i = circuit->createNet("dat_i");
        dat_o = circuit->createNet("dat_o");
        we = circuit->createNet("we");
        sel = circuit->createNet("sel");
        stb = circuit->createNet("stb");
        cyc = circuit->createNet("cyc");
        rx = circuit->createNet("rx");
        ack = circuit->createNet("ack");
        tx = circuit->createNet("tx");
        irq = circuit->createNet("irq");
        circuit->connect(uart, "clk", clk);
        circuit->connect(uart, "wb_adr_i", adr);
        circuit->connect(uart, "wb_dat_i", dat_i);
        circuit->connect(uart, "wb_we_i", we);
        circuit->connect(uart, "wb_sel_i", sel);
        circuit->connect(uart, "wb_stb_i", stb);
        circuit->connect(uart, "wb_cyc_i", cyc);
        circuit->connect(uart, "rx", loopback ? tx : rx); // 回环时 tx 直接接 rx
        circuit->connect(uart, "wb_dat_o", dat_o);
        circuit->connect(uart, "wb_ack_o", ack);
        circuit->connect(uart, "tx", tx);
        circuit->connect(uart, "irq", irq);
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), 0);
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        circuit->setWire(we->id(), 0);
        circuit->setWire(rx->id(), 1); // 串口空闲为高
        for (int i = 0; i < 2; ++i)       // 让 JIT 输出传播到线网，再开始断言
            cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), 0);
        circuit->tick();
        circuit->setWire(clk->id(), 1);
        circuit->tick();
    }
    void write(uint32_t addr, uint32_t v) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(dat_i->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
                                      (uint8_t)((v >> 24) & 0xFF)});
        circuit->setWire(we->id(), 1);
        circuit->setWire(sel->id(), 0x0F);
        circuit->setWire(stb->id(), 1);
        circuit->setWire(cyc->id(), 1);
        for (int i = 0; i < 4; ++i) cycle();
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; ++i) cycle();
    }
    uint32_t read(uint32_t addr) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(we->id(), 0);
        circuit->setWire(sel->id(), 0x0F);
        circuit->setWire(stb->id(), 1);
        circuit->setWire(cyc->id(), 1);
        for (int i = 0; i < 6; ++i) cycle();
        auto v = circuit->getWire(dat_o->id());
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; ++i) cycle();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }
    bool txBit() { return circuit->getWire(tx->id())[0] != 0; }
};
} // namespace

TEST(UartTest, LoopbackByte) {
    Fixture f(2, true); // tx 接回 rx
    f.compile();
    f.write(0x00, 0x5A); // 0101 1010
    for (int i = 0; i < 60; ++i) // 10 位 × 2 tick，留足余量
        f.cycle();
    EXPECT_EQ(f.read(0x00), 0x5Au) << "回环收到的字节应与发送的一致";
}

TEST(UartTest, TransmitPullsLineLow) {
    Fixture f(4);
    f.compile();
    EXPECT_TRUE(f.txBit()) << "空闲时串口线应为高";
    f.write(0x00, 0x41);
    bool sawLow = false;
    for (int i = 0; i < 40; ++i) {
        f.cycle();
        if (!f.txBit())
            sawLow = true;
    }
    EXPECT_TRUE(sawLow) << "发送期间应出现起始位/数据 0，把线拉低";
}

TEST(UartTest, StatusReflectsBusy) {
    Fixture f(2);
    f.compile();
    EXPECT_EQ(f.read(0x14) & 0x20u, 0x20u) << "空闲时 LSR.THRE 应就绪";
    f.write(0x00, 0x55);
    EXPECT_EQ(f.read(0x14) & 0x20u, 0u) << "发送中 LSR.THRE 应为 0";
}

TEST(UartTest, ReceiveSamplesByte) {
    Fixture f(8); // 每位 8 tick，采样点落在位中点
    f.compile();
    // 线上送 0x5A：起始位 0，然后 LSB 先 0,1,0,1,1,0,1,0，停止位 1
    const int bits[10] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 1};
    for (int i = 0; i < 10; ++i) {
        f.circuit->setWire(f.rx->id(), (uint8_t) bits[i]);
        for (int k = 0; k < 4; ++k) // 4 个 cycle = 8 tick
            f.cycle();
    }
    f.circuit->setWire(f.rx->id(), 1);
    f.cycle();
    EXPECT_EQ(f.read(0x14) & 1u, 1u) << "应置 LSR.DR";
    EXPECT_EQ(f.read(0x00), 0x5Au) << "读回收到的字节";
    EXPECT_EQ(f.read(0x14) & 1u, 0u) << "读走 RBR 后 DR 应清除";
}

TEST(UartTest, DlabSwitchesDllDlm) {
    Fixture f(2);
    f.compile();
    f.write(0x0C, 0x80); // LCR.DLAB=1
    f.write(0x00, 0x12); // DLL
    f.write(0x04, 0x34); // DLM
    EXPECT_EQ(f.read(0x00), 0x12u);
    EXPECT_EQ(f.read(0x04), 0x34u);
    f.write(0x0C, 0x03); // DLAB=0，0x04 回到 IER
    EXPECT_EQ(f.read(0x04), 0u);
}

TEST(UartTest, McrAndScrReadBack) {
    Fixture f(2);
    f.compile();
    f.write(0x10, 0x1F);
    f.write(0x1C, 0xA5);
    EXPECT_EQ(f.read(0x10), 0x1Fu);
    EXPECT_EQ(f.read(0x1C), 0xA5u);
}

TEST(UartTest, IirReportsNoPendingInterrupt) {
    Fixture f(2);
    f.compile();
    uint32_t iir = f.read(0x08);
    EXPECT_EQ(iir & 1u, 1u) << "无中断源时 IIR[0]=1";
    EXPECT_EQ(iir & 0xC0u, 0u) << "不实现 FIFO，IIR 不能声称 FIFO 使能";
}

TEST(UartTest, IirThriWhenTxIdleAndEnabled) {
    Fixture f(2);
    f.compile();
    f.write(0x04, 0x02); // IER.THRI
    EXPECT_EQ(f.read(0x08) & 0x0F, 0x02u);
}

TEST(UartTest, IirRdaTakesPriorityOverThri) {
    Fixture f(8);
    f.compile();
    f.write(0x0C, 0x03);
    f.write(0x04, 0x03); // RX 和 TX 中断都开
    const int bits[10] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 1};
    for (int i = 0; i < 10; ++i) {
        f.circuit->setWire(f.rx->id(), (uint8_t) bits[i]);
        for (int k = 0; k < 4; ++k)
            f.cycle();
    }
    f.circuit->setWire(f.rx->id(), 1);
    f.cycle();
    EXPECT_EQ(f.read(0x08) & 0x0F, 0x04u) << "RX 数据可用时 IIR 应报 RDA";
}

TEST(UartTest, LsrTemtWhenIdle) {
    Fixture f(2);
    f.compile();
    EXPECT_EQ(f.read(0x14) & 0x60u, 0x60u) << "空闲时 THRE 和 TEMT 都应置位";
}

TEST(UartTest, UnknownRegisterReadsZero) {
    Fixture f(2);
    f.compile();
    EXPECT_EQ(f.read(0x70), 0u);
}

// 停止位为低是帧错误：数据不进 FIFO，LSR.FE 置位；FE 读 LSR 后清除
TEST(UartTest, LsrFramingError) {
    Fixture f(8);
    f.compile();
    // 起始位 0，数据 0x5A（LSB 先），停止位 0
    const int bits[10] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0};
    for (int i = 0; i < 10; ++i) {
        f.circuit->setWire(f.rx->id(), (uint8_t) bits[i]);
        for (int k = 0; k < 4; ++k)
            f.cycle();
    }
    f.circuit->setWire(f.rx->id(), 1);
    f.cycle();
    uint32_t lsr = f.read(0x14);
    EXPECT_EQ(lsr & 0x08u, 0x08u) << "停止位为低应置 LSR.FE";
    EXPECT_EQ(lsr & 0x01u, 0u) << "帧错误的数据不能进接收 FIFO";
    EXPECT_EQ(f.read(0x14) & 0x08u, 0u) << "FE 读 LSR 后应清除";
}

// 16550A 的 THRE 中断是锁存的：读 IIR（源为 THRE）会清除，THR 再次写空才重新置位
TEST(UartTest, IirThreReadClearsPending) {
    Fixture f(2);
    f.compile();
    f.write(0x04, 0x02); // IER.THRI
    for (int i = 0; i < 4; ++i)
        f.cycle();
    EXPECT_EQ(f.circuit->getWire(f.irq->id())[0], 1) << "THR 空且 THRI 使能时应请求中断";
    EXPECT_EQ(f.read(0x08) & 0x0F, 0x02u);
    EXPECT_EQ(f.circuit->getWire(f.irq->id())[0], 0) << "读 IIR 清除 THRE 中断后 IRQ 应撤销";
    // 再写一个字节并等它发完，完成后 THRE 中断重新挂起
    f.write(0x00, 0x55);
    for (int i = 0; i < 40; ++i)
        f.cycle();
    EXPECT_EQ(f.circuit->getWire(f.irq->id())[0], 1) << "发送完成后 THRE 中断应重新置起";
}

