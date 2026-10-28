// 多级总线级联：组合元件读总线 A、输出又驱动总线 B，B 再被下一级读取。
// 单轮补跑会漏掉第二级，这里验证迭代到不动点的行为。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>

using namespace dsc;

TEST(BusCascade, TwoLevelCascadeSettles) {
    Circuit c;
    auto *a = c.createNet("A");
    auto *b = c.createNet("B");

    // 总线 A：一个三态门常开，驱动 0x55
    auto *tsA = c.addComponent(std::make_unique<GateTSBUF>("tsA", 8));
    // X 读 A 取反，输出经 tsX 驱动总线 B
    auto *x = c.addComponent(std::make_unique<UnaryGate>("X", 8, true));
    auto *tsX = c.addComponent(std::make_unique<GateTSBUF>("tsX", 8));
    // Y 读 B 取反，输出到普通线网
    auto *y = c.addComponent(std::make_unique<UnaryGate>("Y", 8, true));

    auto *na = c.createNet("na"), *nx = c.createNet("nx"), *nout = c.createNet("nout");
    auto *en = c.createNet("en");  // 两个三态门的使能，独立线网，测试里置 1
    c.connect(tsA, "in", na);
    c.connect(tsA, "out", a);
    c.connect(tsA, "oe", en);
    c.connect(x, "in", a);
    c.connect(x, "out", nx);
    c.connect(tsX, "in", nx);
    c.connect(tsX, "out", b);
    c.connect(tsX, "oe", en);
    c.connect(y, "in", b);
    c.connect(y, "out", nout);

    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    c.setWire(na->id(), {0x55, 0});
    c.setWire(en->id(), {1, 0});
    c.tick();

    // A = 0x55，X = ~A = 0xAA，B = 0xAA，Y = ~B = 0x55
    EXPECT_EQ(c.getWire(a->id())[0], 0x55);
    EXPECT_EQ(c.getWire(b->id())[0], 0xAA) << "第二级总线必须看到第一级元件的本拍结果";
    EXPECT_EQ(c.getWire(nout->id())[0], 0x55) << "第二级组合元件必须看到第二级总线的本拍结果";
}
