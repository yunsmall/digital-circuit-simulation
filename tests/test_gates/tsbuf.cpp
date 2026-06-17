// 三态缓冲器测试
#include "common.h"

TEST(GatesTest, TSBUF) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *oe = c.createNet("oe"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateTSBUF>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "oe", oe);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(oe->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xAB);
}

TEST(GatesTest, TSBUF_Disabled) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *oe = c.createNet("oe"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateTSBUF>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "oe", oe);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(oe->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
}

TEST(GatesTest, TSBUF_Bus_TwoDrivers) {
    dsc::Circuit c;
    auto *ina = c.createNet("ina"), *oea = c.createNet("oea");
    auto *inb = c.createNet("inb"), *oeb = c.createNet("oeb");
    auto *bus = c.createNet("bus");
    auto *ta = c.addComponent(std::make_unique<dsc::GateTSBUF>("ta", 8));
    auto *tb = c.addComponent(std::make_unique<dsc::GateTSBUF>("tb", 8));
    c.connect(ta, "in", ina);
    c.connect(ta, "oe", oea);
    c.connect(ta, "out", bus);
    c.connect(tb, "in", inb);
    c.connect(tb, "oe", oeb);
    c.connect(tb, "out", bus);
    c.compile();
    c.init();
    // A 驱动
    c.setWire(ina->id(), {0xAB, 0});
    c.setWire(oea->id(), {1, 0});
    c.setWire(inb->id(), {0xCD, 0});
    c.setWire(oeb->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0xAB);
    // B 驱动
    c.setWire(oea->id(), {0, 0});
    c.setWire(oeb->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0xCD);
    // 两个都关闭
    c.setWire(oea->id(), {0, 0});
    c.setWire(oeb->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0x00);
}

TEST(GatesTest, TSBUF_Bus_Conflict) {
    dsc::Circuit c;
    auto *ina = c.createNet("ina"), *oea = c.createNet("oea");
    auto *inb = c.createNet("inb"), *oeb = c.createNet("oeb");
    auto *bus = c.createNet("bus");
    auto *ta = c.addComponent(std::make_unique<dsc::GateTSBUF>("ta", 8));
    auto *tb = c.addComponent(std::make_unique<dsc::GateTSBUF>("tb", 8));
    c.connect(ta, "in", ina);
    c.connect(ta, "oe", oea);
    c.connect(ta, "out", bus);
    c.connect(tb, "in", inb);
    c.connect(tb, "oe", oeb);
    c.connect(tb, "out", bus);
    c.compile();
    c.init();
    // 两个同时驱动 → 冲突
    c.setWire(ina->id(), {0xAB, 0});
    c.setWire(oea->id(), {1, 0});
    c.setWire(inb->id(), {0xCD, 0});
    c.setWire(oeb->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0x00);

    EXPECT_TRUE(c.hasError());
    EXPECT_EQ(c.tickError().code, dsc::ErrorCode::BUS_CONFLICT);

    // 有错时再 tick 直接返回
    c.setWire(ina->id(), {0x55, 0});
    c.setWire(oea->id(), {1, 0});
    c.setWire(oeb->id(), {0, 0});
    c.tick();
    EXPECT_TRUE(c.hasError());

    // 清空后继续
    c.clearError();
    EXPECT_FALSE(c.hasError());
    c.tick();
    EXPECT_FALSE(c.hasError());
    EXPECT_EQ(c.getWire(bus->id())[0], 0x55);
}
