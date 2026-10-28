// UART 基本行为：发送位序列、接收采样、状态寄存器
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/UART.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    UART *uart = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *dat_o = nullptr;
    Net *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
    Net *rx = nullptr, *ack = nullptr, *tx = nullptr;

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
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), {0});
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        circuit->setWire(we->id(), {0});
        circuit->setWire(rx->id(), {1}); // 串口空闲为高
        for (int i = 0; i < 2; ++i)       // 让 JIT 输出传播到线网，再开始断言
            cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), {0});
        circuit->tick();
        circuit->setWire(clk->id(), {1});
        circuit->tick();
    }
    void write(uint32_t addr, uint32_t v) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(dat_i->id(), {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
                                      (uint8_t)((v >> 24) & 0xFF)});
        circuit->setWire(we->id(), {1});
        circuit->setWire(sel->id(), {0x0F});
        circuit->setWire(stb->id(), {1});
        circuit->setWire(cyc->id(), {1});
        for (int i = 0; i < 4; ++i) cycle();
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        for (int i = 0; i < 2; ++i) cycle();
    }
    uint32_t read(uint32_t addr) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), 0, 0, 0});
        circuit->setWire(we->id(), {0});
        circuit->setWire(sel->id(), {0x0F});
        circuit->setWire(stb->id(), {1});
        circuit->setWire(cyc->id(), {1});
        for (int i = 0; i < 6; ++i) cycle();
        auto v = circuit->getWire(dat_o->id());
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
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
    EXPECT_EQ(f.read(0x04), 0x5Au) << "回环收到的字节应与发送的一致";
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
    EXPECT_EQ(f.read(0x08) & 1u, 1u) << "空闲时 TX 应就绪";
    f.write(0x00, 0x55);
    EXPECT_EQ(f.read(0x08) & 1u, 0u) << "发送中 TX 应忙";
}

TEST(UartTest, ReceiveSamplesByte) {
    Fixture f(8); // 每位 8 tick，采样点落在位中点
    f.compile();
    // 线上送 0x5A：起始位 0，然后 LSB 先 0,1,0,1,1,0,1,0，停止位 1
    const int bits[10] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 1};
    for (int i = 0; i < 10; ++i) {
        f.circuit->setWire(f.rx->id(), {(uint8_t) bits[i]});
        for (int k = 0; k < 4; ++k) // 4 个 cycle = 8 tick
            f.cycle();
    }
    f.circuit->setWire(f.rx->id(), {1});
    f.cycle();
    EXPECT_EQ(f.read(0x08) & 2u, 2u) << "应置 RX 有数据";
    EXPECT_EQ(f.read(0x04), 0x5Au) << "读回收到的字节";
    EXPECT_EQ(f.read(0x08) & 2u, 0u) << "读走后标志应清除";
}
