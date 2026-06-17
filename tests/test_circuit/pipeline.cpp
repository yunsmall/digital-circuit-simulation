// 端到端流水线测试
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(EndToEndTest, SimplePipeline) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *n1 = c.createNet("n1"), *dout = c.createNet("dout"), *clk = c.createNet("clk"),
         *out = c.createNet("out");
    auto *not_g = c.addComponent(std::make_unique<dsc::UnaryGate>("g1", 8));
    c.connect(not_g, "in", in);
    c.connect(not_g, "out", n1);
    auto *dff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(dff, "d", n1);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", dout);
    auto *buf = c.addComponent(std::make_unique<dsc::UnaryGate>("g2", 8, false));
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
    c.tick();
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
