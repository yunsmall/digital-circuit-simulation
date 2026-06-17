// JK 触发器测试
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(JKFlipFlopTest, Hold) {
    dsc::Circuit c;
    auto *j = c.createNet("j"), *k = c.createNet("k"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::JKFlipFlop>("ff", 4));
    c.connect(ff, "j", j);
    c.connect(ff, "k", k);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    // J=0 K=0: 保持（初始为0）
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0);
}

TEST(JKFlipFlopTest, Set) {
    dsc::Circuit c;
    auto *j = c.createNet("j"), *k = c.createNet("k"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::JKFlipFlop>("ff", 4));
    c.connect(ff, "j", j);
    c.connect(ff, "k", k);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    // J=1 K=0: 置一
    c.setWire(j->id(), {1, 0});
    c.setWire(k->id(), {0, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0x0F);
}

TEST(JKFlipFlopTest, Reset) {
    dsc::Circuit c;
    auto *j = c.createNet("j"), *k = c.createNet("k"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::JKFlipFlop>("ff", 4));
    c.connect(ff, "j", j);
    c.connect(ff, "k", k);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    // 先置一
    c.setWire(j->id(), {1, 0});
    c.setWire(k->id(), {0, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    // J=0 K=1: 清零
    c.setWire(j->id(), {0, 0});
    c.setWire(k->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0);
}

TEST(JKFlipFlopTest, Toggle) {
    dsc::Circuit c;
    auto *j = c.createNet("j"), *k = c.createNet("k"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::JKFlipFlop>("ff", 4));
    c.connect(ff, "j", j);
    c.connect(ff, "k", k);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    // J=1 K=1: 翻转
    c.setWire(j->id(), {1, 0});
    c.setWire(k->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0x0F);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 0x0F);
}
