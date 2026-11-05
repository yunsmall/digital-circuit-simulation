// 计数器测试
#include <dcs/Circuit.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

TEST(CounterTest, Increment) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 1; i <= 10; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
        EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, i & 0x0F);
    }
}

TEST(CounterTest, AsyncClear) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *clr = c.createNet("clr"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 4, false, false, false, true));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "clr", clr);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 0; i < 3; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
    }
    EXPECT_EQ(c.getWire(q->id())[0] & 0x0F, 3);
    c.setWire(clr->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
}

// 12 位计数器回绕时，最高字节里超出位宽的位必须清掉
TEST(CounterTest, WideWrapClearsExcessBits) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 12));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    for (int i = 0; i < 4096; i++) {
        c.setWire(clk->id(), {1, 0});
        c.tick();
        c.setWire(clk->id(), {0, 0});
        c.tick();
        if (i == 4094) {
            // 回绕前一拍是 0xFFF，不能带出 bit12..15
            auto v = c.getWire(q->id());
            ASSERT_GE(v.size(), 2u);
            EXPECT_EQ(v[0], 0xFF);
            EXPECT_EQ(v[1], 0x0F);
        }
    }
    auto v = c.getWire(q->id());
    ASSERT_GE(v.size(), 2u);
    EXPECT_EQ(v[0], 0x00);
    EXPECT_EQ(v[1], 0x00) << "12 位计数回绕后应归零";
}

// 递减路径：0 减 1 应回绕到 0xFFF，最高字节同样不能留脏位
TEST(CounterTest, WideUnderflowClearsExcessBits) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *ud = c.createNet("ud"), *q = c.createNet("q");
    auto *cnt = c.addComponent(std::make_unique<dsc::Counter>("cnt", 12, false, false, true));
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "updown", ud);
    c.connect(cnt, "q", q);
    c.compile();
    c.init();
    c.setWire(ud->id(), {1, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    auto v = c.getWire(q->id());
    ASSERT_GE(v.size(), 2u);
    EXPECT_EQ(v[0], 0xFF);
    EXPECT_EQ(v[1], 0x0F) << "12 位下溢后 bit12..15 必须是 0";
}
