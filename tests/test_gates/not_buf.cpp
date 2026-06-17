// NOT 和 BUF 门测试
#include "common.h"

TEST(GatesTest, NOT) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 8, true));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xF0);
}

TEST(GatesTest, BUF) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 8, false));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAB);
}
