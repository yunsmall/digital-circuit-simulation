// 不同位宽的时序元件测试：DFF, TFF, JKFF, Register, Latch, Counter, ShiftRegister
#include "common.h"

// ---- DFF ----
TEST(BitWidthDFF, BW_1) {
    dffBitWidthTest(1);
}
TEST(BitWidthDFF, BW_7) {
    dffBitWidthTest(7);
}
TEST(BitWidthDFF, BW_8) {
    dffBitWidthTest(8);
}
TEST(BitWidthDFF, BW_16) {
    dffBitWidthTest(16);
}
TEST(BitWidthDFF, BW_32) {
    dffBitWidthTest(32);
}
TEST(BitWidthDFF, BW_63) {
    dffBitWidthTest(63);
}
TEST(BitWidthDFF, BW_64) {
    dffBitWidthTest(64);
}

// ---- TFF (1-bit 翻转) ----
TEST(BitWidthTFF, BW_1) {
    dsc::Circuit c;
    auto *t = c.createNet("t"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::TFlipFlop>("ff", 1));
    c.connect(ff, "t", t);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 1);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 0);
}

// ---- JKFF (1-bit) ----
TEST(BitWidthJKFF, BW_1) {
    dsc::Circuit c;
    auto *j = c.createNet("j"), *k = c.createNet("k"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::JKFlipFlop>("ff", 1));
    c.connect(ff, "j", j);
    c.connect(ff, "k", k);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(j->id(), {1, 0});
    c.setWire(k->id(), {0, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 1);
}

// ---- Register (64-bit) ----
TEST(BitWidthReg, BW_64) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *reg = c.addComponent(std::make_unique<dsc::Register>("reg", 64));
    c.connect(reg, "d", d);
    c.connect(reg, "clk", clk);
    c.connect(reg, "q", q);
    c.compile();
    c.init();
    std::vector<uint8_t> val(16, 0);
    for (int i = 0; i < 8; i++)
        val[i] = 0xAA;
    c.setWire(d->id(), val);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    auto out = c.getWire(q->id());
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], 0xAA) << "Reg64 byte=" << i;
}

// ---- Latch (64-bit) ----
TEST(BitWidthLatch, BW_64) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *en = c.createNet("en"), *q = c.createNet("q");
    auto *lt = c.addComponent(std::make_unique<dsc::Latch>("lt", 64));
    c.connect(lt, "d", d);
    c.connect(lt, "en", en);
    c.connect(lt, "q", q);
    c.compile();
    c.init();
    std::vector<uint8_t> val(16, 0);
    val[0] = 0x55;
    val[7] = 0xAA;
    c.setWire(d->id(), val);
    c.setWire(en->id(), {1, 0});
    c.tick();
    auto out = c.getWire(q->id());
    EXPECT_EQ(out[0], 0x55);
    EXPECT_EQ(out[7], 0xAA);
}

// ---- ShiftRegister (64-bit) ----
TEST(BitWidthSR, BW_64) {
    dsc::Circuit c;
    auto *sin = c.createNet("sin"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *sr = c.addComponent(std::make_unique<dsc::ShiftRegister>("sr", 64));
    c.connect(sr, "sin", sin);
    c.connect(sr, "clk", clk);
    c.connect(sr, "q", q);
    c.compile();
    c.init();
    c.setWire(sin->id(), {1, 0});
    for (int i = 0; i < 64; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    auto out = c.getWire(q->id());
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], 0xFF) << "SR64 byte=" << i;
}

// ---- Counter (8-bit wrap, 1-bit) ----
TEST(BitWidthCounter, BW_8_Wrap) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 8));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 0; i < 256; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    EXPECT_EQ(c.getWire(q->id())[0], 0);
}

TEST(BitWidthCounter, BW_1) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 1));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 1);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 1, 0);
}
