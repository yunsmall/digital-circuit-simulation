// SignExt — 符号扩展测试
#include <dcs/Circuit.h>
#include <dcs/components/SignExt.h>
#include <gtest/gtest.h>

// 辅助：设置线网值（指定宽度）
static void setWireVal(dsc::Circuit &c, int id, uint64_t val, int bw) {
    std::vector<uint8_t> v(16, 0);
    for (int i = 0; i < (bw + 7) / 8; i++)
        v[i] = (val >> (i * 8)) & 0xFF;
    c.setWire(id, v);
}

static uint64_t getWireVal(dsc::Circuit &c, int id, int bw) {
    auto v = c.getWire(id);
    uint64_t r = 0;
    for (int i = 0; i < (bw + 7) / 8 && i < 8; i++)
        r |= (uint64_t) v[i] << (i * 8);
    return r;
}

TEST(SignExt, Positive4to8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *se = c.addComponent(std::make_unique<dsc::SignExt>("se", 4, 8));
    c.connect(se, "in", in);
    c.connect(se, "out", out);
    c.compile();
    c.init();

    setWireVal(c, in->id(), 5, 4); // 0b0101
    c.tick();
    EXPECT_EQ(getWireVal(c, out->id(), 8), 5); // 正数: 高位补0
}

TEST(SignExt, Negative4to8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *se = c.addComponent(std::make_unique<dsc::SignExt>("se", 4, 8));
    c.connect(se, "in", in);
    c.connect(se, "out", out);
    c.compile();
    c.init();

    setWireVal(c, in->id(), 0xB, 4); // 4位: 0b1011 = -5 (有符号)
    c.tick();
    EXPECT_EQ(getWireVal(c, out->id(), 8), 0xFB); // 8位: 0b11111011 = -5
}

TEST(SignExt, Positive8to16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *se = c.addComponent(std::make_unique<dsc::SignExt>("se", 8, 16));
    c.connect(se, "in", in);
    c.connect(se, "out", out);
    c.compile();
    c.init();

    setWireVal(c, in->id(), 0x7F, 8); // 127
    c.tick();
    EXPECT_EQ(getWireVal(c, out->id(), 16), 0x007F);
}

TEST(SignExt, Negative8to16) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *se = c.addComponent(std::make_unique<dsc::SignExt>("se", 8, 16));
    c.connect(se, "in", in);
    c.connect(se, "out", out);
    c.compile();
    c.init();

    setWireVal(c, in->id(), 0x80, 8); // -128
    c.tick();
    EXPECT_EQ(getWireVal(c, out->id(), 16), 0xFF80);
}

TEST(SignExt, FullWidth8to64) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *se = c.addComponent(std::make_unique<dsc::SignExt>("se", 8, 64));
    c.connect(se, "in", in);
    c.connect(se, "out", out);
    c.compile();
    c.init();

    setWireVal(c, in->id(), 0xFF, 8); // -1
    c.tick();
    EXPECT_EQ(getWireVal(c, out->id(), 64), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(SignExt, Zero1to8) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *se = c.addComponent(std::make_unique<dsc::SignExt>("se", 1, 8));
    c.connect(se, "in", in);
    c.connect(se, "out", out);
    c.compile();
    c.init();

    setWireVal(c, in->id(), 1, 1); // sign bit=1 → 负数
    c.tick();
    EXPECT_EQ(getWireVal(c, out->id(), 8), 0xFF);
}
