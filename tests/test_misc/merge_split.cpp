// Merge/Split 测试
#include <dcs/Circuit.h>
#include <dcs/components/Misc.h>
#include <format>
#include <gtest/gtest.h>

TEST(MiscTest, Merge4) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *i2 = c.createNet("in2"), *i3 = c.createNet("in3");
    auto *o = c.createNet("out");
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 4));
    c.connect(mg, "in0", i0);
    c.connect(mg, "in1", i1);
    c.connect(mg, "in2", i2);
    c.connect(mg, "in3", i3);
    c.connect(mg, "out", o);
    c.compile();
    c.init();
    c.setWire(i0->id(), {0, 0});
    c.setWire(i1->id(), {1, 0});
    c.setWire(i2->id(), {0, 0});
    c.setWire(i3->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0x0A);
}

TEST(MiscTest, Merge8) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 8));
    dsc::Net *ins[8];
    for (int i = 0; i < 8; i++) {
        ins[i] = c.createNet(std::format("in{}", i));
        c.connect(mg, std::format("in{}", i), ins[i]);
    }
    c.connect(mg, "out", o);
    c.compile();
    c.init();
    uint8_t bits[8] = {1, 0, 0, 0, 1, 1, 0, 1};
    for (int i = 0; i < 8; i++)
        c.setWire(ins[i]->id(), {bits[i], 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xB1);
}

TEST(MiscTest, Split4) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *o0 = c.createNet("out0"), *o1 = c.createNet("out1");
    auto *o2 = c.createNet("out2"), *o3 = c.createNet("out3");
    auto *sp = c.addComponent(std::make_unique<dsc::Split>("sp", 4));
    c.connect(sp, "in", in);
    c.connect(sp, "out0", o0);
    c.connect(sp, "out1", o1);
    c.connect(sp, "out2", o2);
    c.connect(sp, "out3", o3);
    c.compile();
    c.init();
    c.setWire(in->id(), {0x0A, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o0->id())[0] & 1, 0);
    EXPECT_EQ(c.getWire(o1->id())[0] & 1, 1);
    EXPECT_EQ(c.getWire(o2->id())[0] & 1, 0);
    EXPECT_EQ(c.getWire(o3->id())[0] & 1, 1);
}

TEST(MiscTest, SplitMergeRoundtrip) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *bus = c.createNet("bus"), *out = c.createNet("out");
    auto *sp = c.addComponent(std::make_unique<dsc::Split>("sp", 8));
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 8));
    c.connect(sp, "in", in);
    for (int i = 0; i < 8; i++)
        c.connect(sp, std::format("out{}", i), c.createNet(std::format("b{}", i)));
    for (int i = 0; i < 8; i++)
        c.connect(mg, std::format("in{}", i), c.nets()[3 + i].get());
    c.connect(mg, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xA5, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xA5);
}
