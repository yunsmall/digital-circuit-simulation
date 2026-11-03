// 悬空输入引脚的读取语义：netId() 为 -1，生成代码里的 _w[-1] 必须落在恒零槽位上。
// 这条路径以前是越界读，靠 CodeGen 在线网数组前留的零槽位兜底。
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/io/Gpio.h>
#include <gtest/gtest.h>

static std::vector<uint8_t> wireByte(uint8_t b) {
    std::vector<uint8_t> v(16, 0);
    v[0] = b;
    return v;
}

// 普通组合元件：输入不接线时读 0，不能越界读到别的静态变量
TEST(UnconnectedInput, CombinationalReadsZero) {
    dsc::Circuit c;
    dsc::Net *in = c.createNet("in");
    dsc::Net *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("and2", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in); // in1 故意悬空
    c.connect(g, "out", out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.setWire(in->id(), wireByte(0xFF));
    c.tick();
    EXPECT_EQ(c.getWire(out->id()), wireByte(0x00)) << "悬空输入应读 0，0xFF & 0 == 0";
}

// 平台元件大量直接拼 _w[nid]：所有输入都不接时也必须能编译并跑出确定值
TEST(UnconnectedInput, PlatformComponentRuns) {
    dsc::Circuit c;
    dsc::Net *rdata = c.createNet("rdata");
    dsc::Net *ready = c.createNet("ready");
    dsc::Net *gpio_out = c.createNet("gpio_out");
    auto *g = c.addComponent(std::make_unique<dsc::Gpio>("gpio0"));
    c.connect(g, "rdata", rdata);
    c.connect(g, "ready", ready);
    c.connect(g, "gpio_out", gpio_out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    for (int i = 0; i < 4; i++)
        c.tick();
    EXPECT_EQ(c.getWire(gpio_out->id()), wireByte(0x00)) << "悬空输入读 0 后 GPIO 输出保持 0";
    EXPECT_EQ(c.getWire(ready->id()), wireByte(0x00));
}

// 悬空输出：元件照常计算，但不能写 _w[-1]（写了就会破坏零槽位）
TEST(UnconnectedInput, UnconnectedOutputDoesNotCorruptZeroSlot) {
    dsc::Circuit c;
    dsc::Net *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("not0", 8, true));
    c.connect(g, "out", out); // 输入不接
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.tick();
    EXPECT_EQ(c.getWire(out->id()), wireByte(0xFF)) << "~0 应为 0xFF，说明输入读到了 0";
}
