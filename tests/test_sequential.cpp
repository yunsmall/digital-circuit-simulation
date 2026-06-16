#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(DFlipFlopTest, RisingEdge) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    c.setWire(d->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(clk->id(), {0, 0});
    c.setWire(d->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB); // 保持
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x55);
}

TEST(DFlipFlopTest, AsyncReset) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *rst = c.createNet("rst"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8, false, true));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "rst", rst);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(d->id(), {0xAB, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(rst->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00);
    c.setWire(rst->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
}

TEST(TFlipFlopTest, Toggle) {
    dsc::Circuit c;
    auto *t = c.createNet("t"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::TFlipFlop>("tf", 4));
    c.connect(ff, "t", t);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x0F);
    c.setWire(t->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x0F); // 不翻转
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00); // 翻转回0
}

TEST(CounterTest, Increment) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 1; i <= 10; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
        EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, i & 0x0F);
    }
}

TEST(CounterTest, AsyncClear) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *clr = c.createNet("clr"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4, false, false, false, true));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "clr", clr);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 0; i < 3; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 3);
    c.setWire(clr->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
}

TEST(LatchTest, Transparent) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *en = c.createNet("en"), *q = c.createNet("q");
    auto *lt = c.addComponent(std::make_unique<dsc::Latch>("lt", 8));
    c.connect(lt, "d", d);
    c.connect(lt, "en", en);
    c.connect(lt, "q", q);
    c.compile();
    c.init();
    c.setWire(en->id(), {1, 0});
    c.setWire(d->id(), {0xAB, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(en->id(), {0, 0});
    c.setWire(d->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB); // 锁存旧值
    c.setWire(en->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x55);
}

TEST(CircuitRebuildTest, Rebuild) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *in1 = c.createNet("in1"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateAND>("g", 2, 8));
    c.connect(g, "in0", in0);
    c.connect(g, "in1", in1);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xFF, 0});
    c.setWire(in1->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x0F);

    // 清空重建为 OR
    c.clear();
    in0 = c.createNet("in0");
    in1 = c.createNet("in1");
    out = c.createNet("out");
    g = c.addComponent(std::make_unique<dsc::GateOR>("g", 2, 8));
    c.connect(g, "in0", in0);
    c.connect(g, "in1", in1);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xF0, 0});
    c.setWire(in1->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xFF);
}
