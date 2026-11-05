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

// 位宽必须在 1..128：Release 下 addInput 的断言不生效，200 位会让
// genOutputWrite 生成 25 字节拷贝，写穿 16 字节的线网槽
TEST(GatesTest, UnaryGateRejectsOutOfRangeWidth) {
    EXPECT_NO_THROW(dsc::UnaryGate("g", 128, false));
    EXPECT_THROW(dsc::UnaryGate("g", 0, false), std::invalid_argument);
    EXPECT_THROW(dsc::UnaryGate("g", 129, true), std::invalid_argument);
    EXPECT_THROW(dsc::UnaryGate("g", 200, false), std::invalid_argument);
}
