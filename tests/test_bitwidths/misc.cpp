// 杂项元件位宽测试：Constant, Switch, Merge, Split
#include "common.h"

// ---- Constant ----
TEST(BitWidthConst, BW_1) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    c.addComponent(std::make_unique<dsc::Constant>("c", 1, 1));
    c.connect(c.componentById(0), "out", o);
    c.compile();
    c.init();
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0] & 1, 1);
}

TEST(BitWidthConst, BW_64) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    c.addComponent(std::make_unique<dsc::Constant>("c", 64, 0xDEADBEEFCAFEBABEULL));
    c.connect(c.componentById(0), "out", o);
    c.compile();
    c.init();
    c.tick();
    auto out = c.getWire(o->id());
    EXPECT_EQ(out[0], 0xBE);
    EXPECT_EQ(out[7], 0xDE);
}

// ---- Switch ----
TEST(BitWidthSwitch, BW_1) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *en = c.createNet("en"), *o = c.createNet("out");
    auto *sw = c.addComponent(std::make_unique<dsc::Switch>("sw", 1));
    c.connect(sw, "in", in);
    c.connect(sw, "en", en);
    c.connect(sw, "out", o);
    c.compile();
    c.init();
    c.setWire(in->id(), {1, 0});
    c.setWire(en->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0] & 1, 1);
    c.setWire(en->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0] & 1, 0);
}

TEST(BitWidthSwitch, BW_64) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *en = c.createNet("en"), *o = c.createNet("out");
    auto *sw = c.addComponent(std::make_unique<dsc::Switch>("sw", 64));
    c.connect(sw, "in", in);
    c.connect(sw, "en", en);
    c.connect(sw, "out", o);
    c.compile();
    c.init();
    std::vector<uint8_t> val(16, 0xFF);
    c.setWire(in->id(), val);
    c.setWire(en->id(), {1, 0});
    c.tick();
    auto out = c.getWire(o->id());
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], 0xFF) << "Switch64 on byte=" << i;
    c.setWire(en->id(), {0, 0});
    c.tick();
    out = c.getWire(o->id());
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], 0) << "Switch64 off byte=" << i;
}

// ---- Merge (1→64, 64 bit) ----
TEST(BitWidthMerge, BW_64) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 64));
    c.connect(mg, "out", o);
    for (int i = 0; i < 64; i++) {
        auto *nx = c.createNet("in" + std::to_string(i));
        c.connect(mg, "in" + std::to_string(i), nx);
    }
    c.compile();
    c.init();
    // 交替设 1：偶数位=1, 奇数位=0
    for (int i = 0; i < 64; i++) {
        auto *net = c.findNet("in" + std::to_string(i));
        c.setWire(net->id(), {static_cast<uint8_t>(i % 2 == 0 ? 1 : 0), 0});
    }
    c.tick();
    auto out = c.getWire(o->id());
    uint8_t expected = 0x55; // 01010101
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], expected) << "Merge64 byte=" << i;
}

// ---- Split (64→1×64) ----
TEST(BitWidthSplit, BW_64) {
    dsc::Circuit c;
    auto *in = c.createNet("in");
    auto *sp = c.addComponent(std::make_unique<dsc::Split>("sp", 64));
    c.connect(sp, "in", in);
    for (int i = 0; i < 64; i++) {
        auto *nx = c.createNet("out" + std::to_string(i));
        c.connect(sp, "out" + std::to_string(i), nx);
    }
    c.compile();
    c.init();
    std::vector<uint8_t> val(16, 0);
    val[0] = 0xAA; // 10101010 (bit0=0, bit1=1, bit2=0, ...)
    c.setWire(in->id(), val);
    c.tick();
    // bit0 = 0, bit1 = 1, bit2 = 0, bit3 = 1, ...
    EXPECT_EQ(c.getWire(c.findNet("out0")->id())[0] & 1, 0);
    EXPECT_EQ(c.getWire(c.findNet("out1")->id())[0] & 1, 1);
    EXPECT_EQ(c.getWire(c.findNet("out2")->id())[0] & 1, 0);
    EXPECT_EQ(c.getWire(c.findNet("out3")->id())[0] & 1, 1);
}

// ---- DelayLine ----
TEST(BitWidthDelay, BW_64) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk"), *out = c.createNet("out");
    auto *dl = c.addComponent(std::make_unique<dsc::DelayLine>("dl", 64, 1));
    c.connect(dl, "in", in);
    c.connect(dl, "clk", clk);
    c.connect(dl, "out", out);
    c.compile();
    c.init();
    std::vector<uint8_t> val(16, 0);
    for (int i = 0; i < 8; i++)
        val[i] = 0xCC;
    c.setWire(in->id(), val);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(in->id(), std::vector<uint8_t>(16, 0));
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    auto outVal = c.getWire(out->id());
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(outVal[i], 0xCC) << "Delay64 byte=" << i;
}
