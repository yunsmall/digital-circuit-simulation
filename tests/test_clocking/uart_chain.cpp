// 串口输出链路端到端：UartFeed → UartTerm → Print。
// 整机启动看不到串口日志时，先用它排除解码/打印环节。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/io/Print.h>
#include <dcs/components/io/UART.h>
#include <dcs/components/io/UartFeed.h>
#include <dcs/components/io/UartTerm.h>
#include <dcs/components/sequential/Sequential.h>

using namespace dsc;

namespace {
struct Chain {
    Circuit c;
    Net *clk = nullptr;
    Net *rx = nullptr;
    Net *data = nullptr;
    Net *valid = nullptr;
    UartFeed *feed = nullptr;
    Print *print = nullptr;
    std::string out;

    Chain() {
        clk = c.createNet("clk");
        rx = c.createNet("rx");
        data = c.createNet("data");
        valid = c.createNet("valid");

        auto *g = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::None));
        feed = static_cast<UartFeed *>(c.addComponent(std::make_unique<UartFeed>("feed", 2, 125000000)));
        auto *term = c.addComponent(std::make_unique<UartTerm>("term", 2, 125000000));
        print = static_cast<Print *>(c.addComponent(std::make_unique<Print>("print", 8, true)));

        c.connect(g, "clk", clk);
        c.connect(feed, "clk", clk);
        c.connect(feed, "rx", rx);
        c.connect(term, "clk", clk);
        c.connect(term, "rx", rx);
        c.connect(term, "data", data);
        c.connect(term, "valid", valid);
        c.connect(print, "in", data);
        c.connect(print, "clk", clk);
        c.connect(print, "en", valid);

        TextSink sink;
        sink.on_text = [](void *ctx, const Component *, const char *d, int n) {
            static_cast<std::string *>(ctx)->append(d, n);
        };
        print->setSink(sink, &out);
    }
};
} // namespace

TEST(UartChain, FeedTermPrint) {
    Chain f;
    f.feed->push('A');
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();

    // 8N1 一帧 10 bit，每 bit 4 个 clk 周期（2 tick）= 80 tick，留足余量
    for (int i = 0; i < 300; i++)
        f.c.tick();
    EXPECT_EQ(f.out, "A");
}

TEST(UartChain, UartTxToPrint) {
    Circuit c;
    Net *clk = c.createNet("clk");
    Net *rx = c.createNet("rx");
    Net *adr = c.createNet("adr");
    Net *wdat = c.createNet("wdat");
    Net *we = c.createNet("we");
    Net *sel = c.createNet("sel");
    Net *stb = c.createNet("stb");
    Net *cyc = c.createNet("cyc");
    Net *dat = c.createNet("dat");
    Net *ack = c.createNet("ack");
    Net *tx = c.createNet("tx");
    Net *irq = c.createNet("irq");
    Net *term_data = c.createNet("term_data");
    Net *term_valid = c.createNet("term_valid");

    auto *g = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::None));
    auto *uart = c.addComponent(std::make_unique<UART>("uart", 2, 125000000));
    auto *term = c.addComponent(std::make_unique<UartTerm>("term", 2, 125000000));
    auto *pr = static_cast<Print *>(c.addComponent(std::make_unique<Print>("print", 8, true)));

    std::string err;
    auto *rx_hi = c.addComponent(
            ComponentFactory::instance().create("constant", "rx_hi", {{"bit_width", "1"}, {"value", "1"}}, err));
    ASSERT_TRUE(rx_hi) << err;

    c.connect(g, "clk", clk);
    c.connect(uart, "clk", clk);
    c.connect(uart, "rx", rx);
    c.connect(uart, "wb_adr_i", adr);
    c.connect(uart, "wb_dat_i", wdat);
    c.connect(uart, "wb_we_i", we);
    c.connect(uart, "wb_sel_i", sel);
    c.connect(uart, "wb_stb_i", stb);
    c.connect(uart, "wb_cyc_i", cyc);
    c.connect(uart, "wb_dat_o", dat);
    c.connect(uart, "wb_ack_o", ack);
    c.connect(uart, "tx", tx);
    c.connect(uart, "irq", irq);
    c.connect(term, "clk", clk);
    c.connect(term, "rx", tx);
    c.connect(term, "data", term_data);
    c.connect(term, "valid", term_valid);
    c.connect(pr, "in", term_data);
    c.connect(pr, "clk", clk);
    c.connect(pr, "en", term_valid);
    c.connect(rx_hi, "out", rx);

    std::string out;
    TextSink sink;
    sink.on_text = [](void *ctx, const Component *, const char *d, int n) {
        static_cast<std::string *>(ctx)->append(d, n);
    };
    pr->setSink(sink, &out);

    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    c.setWire(sel->id(), {0x0F});
    c.setWire(cyc->id(), {1});
    auto bus_write = [&](uint32_t a, uint32_t v) {
        c.setWire(adr->id(), {(uint8_t) a, 0, 0, 0});
        c.setWire(wdat->id(), {(uint8_t) v, 0, 0, 0});
        c.setWire(we->id(), {1});
        c.setWire(stb->id(), {1});
        c.tick();
        c.tick();
        c.setWire(stb->id(), {0});
        c.tick();
        c.tick();
    };

    bus_write(0x0C, 0x03); // LCR: 8N1
    bus_write(0x00, 'A'); // THR
    c.setWire(we->id(), {0});
    c.setWire(cyc->id(), {0});

    for (int i = 0; i < 300; i++)
        c.tick();
    EXPECT_EQ(out, "A") << "UART 的 THR 写入应该从 tx 送出并被打回字符";
}

TEST(UartChain, TwoBytesKeepOrder) {
    Chain f;
    f.feed->push('O');
    f.feed->push('K');
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    for (int i = 0; i < 500; i++)
        f.c.tick();
    EXPECT_EQ(f.out, "OK");
}

