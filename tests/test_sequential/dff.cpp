// D 触发器测试
#include <dcs/Circuit.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

TEST(DFlipFlopTest, RisingEdge) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    c.setWire(d->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(clk->id(), {0, 0});
    c.setWire(d->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x55);
}

TEST(DFlipFlopTest, AsyncReset) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *rst = c.createNet("rst"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8, false, true));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "rst", rst);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    c.setWire(d->id(), {0xAB, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
    c.setWire(rst->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00);
    c.setWire(rst->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xAB);
}

// ---- 可选引脚存在但悬空 ----
// 引脚对象在（has_rst 为真）但没连网表时，生成的 C 代码仍要声明 _rst，
// 否则 update 分支引用未声明变量，JIT 编译直接失败。
TEST(DFlipFlopTest, OptionalPinsLeftUnconnected) {
    dsc::Circuit c;
    auto *d = c.createNet("d");
    auto *clk = c.createNet("clk");
    auto *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8, false, true, true));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    // rst / pre 引脚故意不连

    dsc::CircuitError err = c.compile();
    EXPECT_EQ(static_cast<int>(err.code), 0) << "悬空可选引脚不应让编译失败: " << err.message;
    c.init();

    c.setWire(d->id(), {0x5A, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x5Au) << "悬空 rst/pre 应按无效处理";
}


// ---- 异步置位（has_preset=true）----
// 引脚顺序是 d/clk/[en]/[rst]/[pre]，pre 的索引算错就会从 clk 或 rst 取到值
TEST(DFlipFlopTest, AsyncPreset) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk");
    auto *pre = c.createNet("pre"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8, false, false, true));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "pre", pre);
    c.connect(ff, "q", q);
    c.compile();
    c.init();

    // 先写入 0x00
    c.setWire(d->id(), {0x00, 0});
    c.setWire(clk->id(), {0, 0});
    c.setWire(pre->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0x00u);

    // 拉高 pre：输出应被异步置成全 1
    c.setWire(pre->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xFFu) << "pre 有效时应异步置位";

    // 撤销 pre，且 clk 保持高（无上升沿）——值应保持
    c.setWire(pre->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0xFFu) << "撤销 pre 后应保持置位值";
}

