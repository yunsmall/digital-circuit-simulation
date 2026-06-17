// Print 打印元件测试
// 验证 Print 元件可编译、可仿真、不崩溃
#include <dcs/Circuit.h>
#include <dcs/components/Print.h>
#include <gtest/gtest.h>

TEST(PrintTest, Smoke) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk");
    auto *p = c.addComponent(std::make_unique<dsc::Print>("p", 8));
    c.connect(p, "in", in);
    c.connect(p, "clk", clk);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick(); // 上升沿打印
    c.setWire(in->id(), {0x55, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    SUCCEED();
}
