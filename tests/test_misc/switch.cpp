// 开关元件测试
#include <dcs/Circuit.h>
#include <dcs/components/Misc.h>
#include <gtest/gtest.h>

TEST(MiscTest, SwitchOn) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *en = c.createNet("en"), *o = c.createNet("out");
    auto *sw = c.addComponent(std::make_unique<dsc::Switch>("sw", 8));
    c.connect(sw, "in", in);
    c.connect(sw, "en", en);
    c.connect(sw, "out", o);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(en->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xAB);
}

TEST(MiscTest, SwitchOff) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *en = c.createNet("en"), *o = c.createNet("out");
    auto *sw = c.addComponent(std::make_unique<dsc::Switch>("sw", 8));
    c.connect(sw, "in", in);
    c.connect(sw, "en", en);
    c.connect(sw, "out", o);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(en->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}
