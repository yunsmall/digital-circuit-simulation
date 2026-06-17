// 多路选择器测试
#include <dcs/Circuit.h>
#include <dcs/components/Arithmetic.h>
#include <gtest/gtest.h>

TEST(MuxTest, TwoToOne) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Mux>("m", 1, 8));
    c.connect(g, "sel0", s0);
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(i0->id(), {0xAB, 0});
    c.setWire(i1->id(), {0xCD, 0});
    c.setWire(s0->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xAB);
    c.setWire(s0->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xCD);
}

TEST(MuxTest, FourToOne) {
    dsc::Circuit c;
    auto *s0 = c.createNet("s0"), *s1 = c.createNet("s1"), *i0 = c.createNet("in0"), *i1 = c.createNet("in1"),
         *i2 = c.createNet("in2"), *i3 = c.createNet("in3"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::Mux>("m", 2, 4));
    c.connect(g, "sel0", s0);
    c.connect(g, "sel1", s1);
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "in2", i2);
    c.connect(g, "in3", i3);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    c.setWire(i0->id(), {0x0A, 0});
    c.setWire(i1->id(), {0x0B, 0});
    c.setWire(i2->id(), {0x0C, 0});
    c.setWire(i3->id(), {0x0D, 0});
    c.setWire(s0->id(), {0, 0});
    c.setWire(s1->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0x0C);
    c.setWire(s0->id(), {1, 0});
    c.setWire(s1->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0x0D);
}
