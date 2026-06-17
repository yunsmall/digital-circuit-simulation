// 寄存器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(RegisterTest, BasicStore) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *reg = c.addComponent(std::make_unique<dsc::Register>("reg", 8));
    c.connect(reg, "d", d);
    c.connect(reg, "clk", clk);
    c.connect(reg, "q", q);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    // 上升沿存储
    c.setWire(d->id(), {0x5A, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x5A);
    // 保持
    c.setWire(d->id(), {0xA5, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x5A);
}

TEST(RegisterTest, WithEnable) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *en = c.createNet("en"), *q = c.createNet("q");
    auto *reg = c.addComponent(std::make_unique<dsc::Register>("reg", 8, true));
    c.connect(reg, "d", d);
    c.connect(reg, "clk", clk);
    c.connect(reg, "en", en);
    c.connect(reg, "q", q);
    c.compile();
    c.init();
    // en=0: 不更新
    c.setWire(d->id(), {0x5A, 0});
    c.setWire(en->id(), {0, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    // en=1: 更新
    c.setWire(en->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x5A);
}

TEST(RegisterTest, AsyncReset) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *rst = c.createNet("rst"), *q = c.createNet("q");
    auto *reg = c.addComponent(std::make_unique<dsc::Register>("reg", 8, false, true));
    c.connect(reg, "d", d);
    c.connect(reg, "clk", clk);
    c.connect(reg, "rst", rst);
    c.connect(reg, "q", q);
    c.compile();
    c.init();
    c.setWire(d->id(), {0x5A, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x5A);
    // 异步复位
    c.setWire(rst->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
}
