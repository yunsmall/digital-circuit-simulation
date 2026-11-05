// T 触发器测试
#include <dcs/Circuit.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

TEST(TFlipFlopTest, Toggle) {
    dsc::Circuit c;
    auto *t = c.createNet("t"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::TFlipFlop>("tf", 4));
    c.connect(ff, "t", t);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x0F);
    c.setWire(t->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x0F);
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00);
}

// 位宽不是 8 的倍数时，最高字节里超出位宽的位必须清掉：
// 翻转会把整字节取反，不掩的话 12 位输出的 bit12..15 会变成 1
TEST(TFlipFlopTest, WideToggleClearsExcessBits) {
    dsc::Circuit c;
    auto *t = c.createNet("t"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::TFlipFlop>("tf", 12));
    c.connect(ff, "t", t);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    // 多跑一拍让时序输出进线网
    c.setWire(clk->id(), {0, 0});
    c.tick();
    auto v = c.getWire(q->id());
    ASSERT_GE(v.size(), 2u);
    EXPECT_EQ(v[0], 0xFF);
    EXPECT_EQ(v[1], 0x0F) << "12 位翻转后 bit12..15 必须是 0";
}

// 位宽是 8 的倍数时最高字节整字节有效，不能被误清
TEST(TFlipFlopTest, ByteWidthToggleKeepsAllBits) {
    dsc::Circuit c;
    auto *t = c.createNet("t"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::TFlipFlop>("tf", 16));
    c.connect(ff, "t", t);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(t->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    auto v = c.getWire(q->id());
    ASSERT_GE(v.size(), 2u);
    EXPECT_EQ(v[0], 0xFF);
    EXPECT_EQ(v[1], 0xFF);
}
