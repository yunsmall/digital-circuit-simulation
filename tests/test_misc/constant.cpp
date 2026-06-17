// 常量元件测试
#include <dcs/Circuit.h>
#include <dcs/components/Misc.h>
#include <gtest/gtest.h>

TEST(MiscTest, Constant8) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    c.addComponent(std::make_unique<dsc::Constant>("c", 8, 0xAB));
    c.connect(c.components()[0].get(), "out", o);
    c.compile();
    c.init();
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xAB);
}

TEST(MiscTest, Constant32) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    c.addComponent(std::make_unique<dsc::Constant>("c", 32, 0xDEADBEEF));
    c.connect(c.components()[0].get(), "out", o);
    c.compile();
    c.init();
    c.tick();
    auto v = c.getWire(o->id());
    EXPECT_EQ(v[0], 0xEF);
    EXPECT_EQ(v[1], 0xBE);
    EXPECT_EQ(v[2], 0xAD);
    EXPECT_EQ(v[3], 0xDE);
}
