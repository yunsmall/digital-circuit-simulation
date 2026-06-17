// FloatPrint 浮点打印元件测试
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/FloatPrint.h>
#include <gtest/gtest.h>

TEST(FloatPrintTest, Smoke32) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk");
    auto *fp = c.addComponent(std::make_unique<dsc::FloatPrint>("fp", 32));
    c.connect(fp, "in", in);
    c.connect(fp, "clk", clk);
    c.compile();
    c.init();
    // 写入 2.5f 的 IEEE 754 位模式
    float v = 2.5f;
    uint32_t u;
    std::memcpy(&u, &v, 4);
    std::vector<uint8_t> bytes(4);
    for (int i = 0; i < 4; i++)
        bytes[i] = (u >> (i * 8)) & 0xFF;
    c.setWire(in->id(), bytes);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    SUCCEED();
}

TEST(FloatPrintTest, Smoke64) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk");
    auto *fp = c.addComponent(std::make_unique<dsc::FloatPrint>("fp", 64));
    c.connect(fp, "in", in);
    c.connect(fp, "clk", clk);
    c.compile();
    c.init();
    double v = 3.14159;
    uint64_t u;
    std::memcpy(&u, &v, 8);
    std::vector<uint8_t> bytes(8);
    for (int i = 0; i < 8; i++)
        bytes[i] = (u >> (i * 8)) & 0xFF;
    c.setWire(in->id(), bytes);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    SUCCEED();
}
