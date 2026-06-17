// D 锁存器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(LatchTest, Transparent) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *en = c.createNet("en"), *q = c.createNet("q");
    auto *lt = c.addComponent(std::make_unique<dsc::Latch>("lt", 8));
    c.connect(lt, "d", d);
    c.connect(lt, "en", en);
    c.connect(lt, "q", q);
    c.compile();
    c.init();
    c.setWire(en->id(), {1, 0});
    c.setWire(d->id(), {0xAB, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(en->id(), {0, 0});
    c.setWire(d->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(en->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x55);
}
