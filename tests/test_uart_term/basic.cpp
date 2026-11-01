// UartTerm 基本行为：把 UART 的串行输出解码成字节
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/UART.h>
#include <dcs/components/io/UartTerm.h>

using namespace dsc;

namespace {
struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    UART *uart = nullptr;
    UartTerm *term = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *we = nullptr;
    Net *sel = nullptr, *stb = nullptr, *cyc = nullptr, *ack = nullptr;
    Net *tx = nullptr, *data = nullptr, *valid = nullptr;

    explicit Fixture(int baud = 2) {
        auto u = std::make_unique<UART>("uart0", baud);
        uart = u.get();
        circuit->addComponent(std::move(u));
        auto t = std::make_unique<UartTerm>("term0", baud);
        term = t.get();
        circuit->addComponent(std::move(t));

        clk = circuit->createNet("clk");
        adr = circuit->createNet("adr");
        dat_i = circuit->createNet("dat_i");
        we = circuit->createNet("we");
        sel = circuit->createNet("sel");
        stb = circuit->createNet("stb");
        cyc = circuit->createNet("cyc");
        ack = circuit->createNet("ack");
        tx = circuit->createNet("tx");
        data = circuit->createNet("data");
        valid = circuit->createNet("valid");

        circuit->connect(uart, "clk", clk);
        circuit->connect(uart, "wb_adr_i", adr);
        circuit->connect(uart, "wb_dat_i", dat_i);
        circuit->connect(uart, "wb_we_i", we);
        circuit->connect(uart, "wb_sel_i", sel);
        circuit->connect(uart, "wb_stb_i", stb);
        circuit->connect(uart, "wb_cyc_i", cyc);
        circuit->connect(uart, "wb_ack_o", ack);
        circuit->connect(uart, "tx", tx);
        // 终端只收不发，rx 直接吃 UART 的 tx
        circuit->connect(term, "clk", clk);
        circuit->connect(term, "rx", tx);
        circuit->connect(term, "data", data);
        circuit->connect(term, "valid", valid);
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), 0);
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        circuit->setWire(we->id(), 0);
        for (int i = 0; i < 2; ++i) // 让 seq 输出先传播到线网
            cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), 0);
        circuit->tick();
        circuit->setWire(clk->id(), 1);
        circuit->tick();
    }
    void write(uint32_t addr, uint32_t v) {
        circuit->setWire(adr->id(), {(uint8_t) (addr & 0xFF), 0, 0, 0});
        circuit->setWire(dat_i->id(), {(uint8_t) (v & 0xFF), (uint8_t) ((v >> 8) & 0xFF),
                                       (uint8_t) ((v >> 16) & 0xFF), (uint8_t) ((v >> 24) & 0xFF)});
        circuit->setWire(we->id(), 1);
        circuit->setWire(sel->id(), 0x0F);
        circuit->setWire(stb->id(), 1);
        circuit->setWire(cyc->id(), 1);
        for (int i = 0; i < 4; ++i) cycle();
        circuit->setWire(stb->id(), 0);
        circuit->setWire(cyc->id(), 0);
        for (int i = 0; i < 2; ++i) cycle();
    }
};
// 只放一个 UartTerm，用外部直接驱动 rx 电平来构造合法/非法帧。
// 不能靠断开已有 UART 再重连：那会让输入引脚的 netId 变成 -1，JIT 生成 _w[-1]。
struct TermOnly {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    UartTerm *term = nullptr;
    Net *clk = nullptr, *rx = nullptr, *data = nullptr, *valid = nullptr;

    explicit TermOnly(int baud = 2) {
        auto t = std::make_unique<UartTerm>("term0", baud);
        term = t.get();
        circuit->addComponent(std::move(t));
        clk = circuit->createNet("clk");
        rx = circuit->createNet("rx");
        data = circuit->createNet("data");
        valid = circuit->createNet("valid");
        circuit->connect(term, "clk", clk);
        circuit->connect(term, "rx", rx);
        circuit->connect(term, "data", data);
        circuit->connect(term, "valid", valid);
    }
    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), 0);
        circuit->setWire(rx->id(), (uint8_t) 1); // 串口线空闲为高
        for (int i = 0; i < 2; ++i)
            cycle();
    }
    void cycle() {
        circuit->setWire(clk->id(), 0);
        circuit->tick();
        circuit->setWire(clk->id(), 1);
        circuit->tick();
    }
    void level(int v, int cycles) {
        circuit->setWire(rx->id(), (uint8_t) v);
        for (int i = 0; i < cycles; ++i)
            cycle();
    }
};

} // namespace

TEST(UartTermTest, DecodesSingleByte) {
    Fixture f(2);
    f.compile();
    f.write(0x00, 0x41); // 'A'
    for (int i = 0; i < 60; ++i) // 10 位 × 2 tick，留足余量
        f.cycle();
    EXPECT_EQ(f.term->frames(), 1u) << "应收到恰好一帧";
    EXPECT_EQ(f.term->lastByte(), 0x41u);
    EXPECT_EQ(f.term->errors(), 0u);
}

TEST(UartTermTest, DecodesConsecutiveBytes) {
    Fixture f(2);
    f.compile();
    // 前一个字节还在发送时 THR 不接受新数据，每写一个都要等它发完
    for (char ch: std::string("OK\n")) {
        f.write(0x00, (uint32_t) (unsigned char) ch);
        for (int i = 0; i < 40; ++i)
            f.cycle();
    }
    EXPECT_EQ(f.term->frames(), 3u) << "三个字节应各自解成一帧";
    EXPECT_EQ(f.term->lastByte(), (uint8_t) '\n');
    EXPECT_EQ(f.term->errors(), 0u);
}

TEST(UartTermTest, IdleLineProducesNothing) {
    Fixture f(2);
    f.compile();
    for (int i = 0; i < 100; ++i)
        f.cycle();
    EXPECT_EQ(f.term->frames(), 0u) << "空闲线为高，不应把它当成帧";
    EXPECT_EQ(f.term->errors(), 0u);
}

TEST(UartTermTest, FrameErrorWhenStopBitLow) {
    TermOnly f(2);
    f.compile();
    f.level(1, 4);  // 空闲高
    f.level(0, 2);  // 起始位
    for (int b = 0; b < 8; ++b)
        f.level(0, 2); // 数据位全 0
    f.level(0, 4);     // 停止位保持低 → 帧错误
    EXPECT_EQ(f.term->frames(), 0u) << "停止位为低不能算有效帧";
    EXPECT_EQ(f.term->errors(), 1u) << "应记一次帧错误";
}

TEST(UartTermTest, NarrowGlitchRejected) {
    // baud=16 时半位确认点是 8 tick，而一个 level(0,1) 只有 2 tick，够不成起始位
    TermOnly f(16);
    f.compile();
    f.level(1, 4);
    f.level(0, 1); // 窄脉冲：只有一拍（2 tick）低电平
    f.level(1, 40);
    EXPECT_EQ(f.term->frames(), 0u) << "噪声不应解成字节";
    EXPECT_EQ(f.term->errors(), 0u) << "噪声也不该记成帧错误";
}
