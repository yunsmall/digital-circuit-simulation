#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(CircuitCheckTest, CombinationalLoopDetected) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g = c.addComponent(std::make_unique<dsc::GateNOT>("g", 8));
    c.connect(g, "in", w);
    c.connect(g, "out", w);
    EXPECT_THROW(c.check(), std::runtime_error);
}

TEST(CircuitCheckTest, NoLoopWithSequential) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *d = c.createNet("d"), *q = c.createNet("q");
    auto *not_g = c.addComponent(std::make_unique<dsc::GateNOT>("not", 8));
    c.connect(not_g, "in", q);
    c.connect(not_g, "out", d);
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    EXPECT_NO_THROW(c.check());
}

TEST(CircuitCheckTest, MultipleDriversError) {
    dsc::Circuit c;
    auto *w = c.createNet("w"), *in = c.createNet("in");
    auto *g1 = c.addComponent(std::make_unique<dsc::GateBUF>("b1", 8));
    auto *g2 = c.addComponent(std::make_unique<dsc::GateBUF>("b2", 8));
    c.connect(g1, "in", in);
    c.connect(g2, "in", in);
    c.connect(g1, "out", w);
    EXPECT_THROW(c.connect(g2, "out", w), std::runtime_error);
}

TEST(WireTest, BitWidthDeterminedByMaxPin) {
    dsc::Circuit c;
    auto *w = c.createNet("w"), *in = c.createNet("in");
    auto *g4 = c.addComponent(std::make_unique<dsc::GateBUF>("b4", 4));
    auto *g8 = c.addComponent(std::make_unique<dsc::GateBUF>("b8", 8));
    c.connect(g4, "in", in);
    c.connect(g4, "out", w);
    EXPECT_EQ(w->bit_width(), 4);
    c.connect(g8, "in", w);
    EXPECT_EQ(w->bit_width(), 8);
}

TEST(EndToEndTest, SimplePipeline) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *n1 = c.createNet("n1"), *dout = c.createNet("dout"), *clk = c.createNet("clk"),
         *out = c.createNet("out");
    auto *not_g = c.addComponent(std::make_unique<dsc::GateNOT>("g1", 8));
    c.connect(not_g, "in", in);
    c.connect(not_g, "out", n1);
    auto *dff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(dff, "d", n1);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", dout);
    auto *buf = c.addComponent(std::make_unique<dsc::GateBUF>("g2", 8));
    c.connect(buf, "in", dout);
    c.connect(buf, "out", out);

    c.compile();
    c.init();
    c.setWire(in->id(), {0x0F, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(n1->id())[0], 0xF0);
    EXPECT_EQ(c.getWire(dout->id())[0], 0x00);

    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xF0);
    c.tick(); // 再一tick让BUF看到新值
    EXPECT_EQ(c.getWire(out->id())[0], 0xF0);

    c.setWire(in->id(), {0x55, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAA);
}

TEST(EndToEndTest, CounterWithGateFeedback) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 0; i < 8; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 8);
}

TEST(ErrorTest, UnconnectedPinIsZero) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateAND>("g", 2, 8));
    c.connect(g, "in0", in0);
    c.connect(g, "out", out); // in1 悬空
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
}

TEST(ErrorTest, InvalidGateInputs) {
    EXPECT_THROW(dsc::GateAND("g", 0, 8), std::invalid_argument);
    EXPECT_THROW(dsc::GateAND("g", 9, 8), std::invalid_argument);
    EXPECT_THROW(dsc::GateAND("g", 2, 129), std::invalid_argument);
}
