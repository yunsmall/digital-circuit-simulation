// uart_feed 的输出接到 UART 的 rx，验证"宿主字符 → 串行线 → 8250 接收寄存器"整条链路
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/UART.h>
#include <dcs/components/io/UartFeed.h>

using namespace dsc;

namespace {

struct FeedToUartFixture {
    std::unique_ptr<Circuit> c;
    UartFeed *feed = nullptr;
    UART *uart = nullptr;
    Net *clk, *tx_line, *adr, *wdat, *we, *sel, *stb, *cyc, *rdat, *ack;

    FeedToUartFixture() {
        c = std::make_unique<Circuit>();
        auto f = std::make_unique<UartFeed>("feed0", 8);
        feed = f.get();
        c->addComponent(std::move(f));
        auto u = std::make_unique<UART>("uart0", 8);
        uart = u.get();
        c->addComponent(std::move(u));

        clk = c->createNet("clk");
        tx_line = c->createNet("tx_line"); // feed 的 rx 输出 → UART 的 rx 输入
        adr = c->createNet("adr");
        wdat = c->createNet("wdat");
        we = c->createNet("we");
        sel = c->createNet("sel");
        stb = c->createNet("stb");
        cyc = c->createNet("cyc");
        rdat = c->createNet("rdat");
        ack = c->createNet("ack");
        Net *uart_tx = c->createNet("uart_tx");
        Net *uart_irq = c->createNet("uart_irq");

        c->connect(feed, "clk", clk);
        c->connect(feed, "rx", tx_line);
        c->connect(uart, "clk", clk);
        c->connect(uart, "rx", tx_line);
        for (auto [pin, net] : {std::pair{"wb_adr_i", adr}, {"wb_dat_i", wdat}, {"wb_we_i", we},
                                {"wb_sel_i", sel}, {"wb_stb_i", stb}, {"wb_cyc_i", cyc},
                                {"wb_dat_o", rdat}, {"wb_ack_o", ack}, {"tx", uart_tx}, {"irq", uart_irq}})
            c->connect(uart, pin, net);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
    }

    void ticks(int n) {
        for (int i = 0; i < n; i++) c->tick();
    }
    static void set32(Circuit *c, Net *n, uint32_t v) {
        c->setWire(n->id(), {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)});
    }
    uint32_t get32(Net *n) const {
        auto v = c->getWire(n->id());
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    uint32_t readReg(uint32_t a) {
        set32(c.get(), adr, a);
        c->setWire(we->id(), 0);
        c->setWire(sel->id(), 0x0F);
        c->setWire(stb->id(), 1);
        c->setWire(cyc->id(), 1);
        ticks(4);
        uint32_t v = get32(rdat);
        c->setWire(stb->id(), 0);
        c->setWire(cyc->id(), 0);
        ticks(2);
        return v;
    }
};

} // namespace

TEST(UartFeedToUartTest, ByteArrivesInReceiveRegister) {
    FeedToUartFixture f;
    // 先让串行线稳定在空闲高电平：UART 靠"高→低"的下降沿识别起始位，
    // 复位后第一拍线网还是 0，直接 push 会丢掉第一个起始沿
    f.ticks(10);
    ASSERT_TRUE(f.feed->push('A'));
    // 8N1 @ baud_div=8：起始位 + 8 数据位 + 停止位 ≈ 80 拍，留足余量
    f.ticks(400);

    const uint32_t lsr = f.readReg(0x14);
    EXPECT_TRUE((lsr & 1u) != 0) << "LSR.DR 没有置位，UART 没收到字节";
    const uint32_t rbr = f.readReg(0x00);
    EXPECT_EQ(rbr & 0xFFu, (uint32_t) 'A') << "收到的字节不对";
}

TEST(UartFeedToUartTest, TwoBytesArriveInOrder) {
    FeedToUartFixture f;
    f.ticks(10);
    // UART 有 16 字节接收 FIFO，连续两帧都应该保留下来
    ASSERT_TRUE(f.feed->push((const uint8_t *) "hi", 2));
    f.ticks(400);
    EXPECT_EQ(f.readReg(0x00) & 0xFFu, (uint32_t) 'h');
    EXPECT_EQ(f.readReg(0x00) & 0xFFu, (uint32_t) 'i');
}
