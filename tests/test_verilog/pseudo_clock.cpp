// Verilator DLL 包装层的伪时钟合成：clk 接 "每 tick 一个上升沿" 的 clkgen
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/platform/Dll.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

#include "common/dll_name.h"

using namespace dsc;

namespace {

std::unique_ptr<Component> constant(const std::string &name, int value) {
    std::string err;
    auto c = ComponentFactory::instance().create(
            "constant", name, {{"bit_width", "1"}, {"value", std::to_string(value)}}, err);
    EXPECT_TRUE(c) << err;
    return c;
}

} // namespace

TEST(VerilatorPseudoClock, DualPseudoClocksToggleTogether) {
    // 同一拍里两个伪时钟统一翻转：上升沿和下降沿各自计数，互不干扰
    Circuit c;
    Net *clk_a = c.createNet("clk_a");
    Net *clk_b = c.createNet("clk_b");
    Net *rst = c.createNet("rst");
    Net *count_a = c.createNet("count_a");
    Net *count_b = c.createNet("count_b");

    auto *ga = c.addComponent(std::make_unique<ClockGen>("ga", 1, 1, PinEdge::Rising));
    auto *gb = c.addComponent(std::make_unique<ClockGen>("gb", 1, 1, PinEdge::Falling));
    auto *cnt = c.addComponent(std::make_unique<DllComponent>("cnt", dllFileName("dual_clk_dll")));
    ASSERT_NE(cnt, nullptr);
    auto *rst_c = c.addComponent(constant("rst_c", 0));

    c.connect(ga, "clk", clk_a);
    c.connect(gb, "clk", clk_b);
    c.connect(cnt, "clk_a", clk_a);
    c.connect(cnt, "clk_b", clk_b);
    c.connect(cnt, "rst", rst);
    c.connect(cnt, "count_a", count_a);
    c.connect(cnt, "count_b", count_b);
    c.connect(rst_c, "out", rst);

    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    c.init();

    for (int i = 0; i < 5; i++)
        c.tick();
    EXPECT_EQ(c.getWire(count_a->id())[0], 5u);
    EXPECT_EQ(c.getWire(count_b->id())[0], 5u);
}

TEST(VerilatorPseudoClock, MixedPseudoAndRealClock) {
    // clk_a 是伪时钟（每 tick 一个上升沿），clk_b 是普通 1/1 时钟；
    // 5 tick 内 clk_b 只有 tick3、tick5 两个下降沿
    Circuit c;
    Net *clk_a = c.createNet("clk_a");
    Net *clk_b = c.createNet("clk_b");
    Net *rst = c.createNet("rst");
    Net *count_a = c.createNet("count_a");
    Net *count_b = c.createNet("count_b");

    auto *ga = c.addComponent(std::make_unique<ClockGen>("ga", 1, 1, PinEdge::Rising));
    auto *gb = c.addComponent(std::make_unique<ClockGen>("gb", 1, 1, PinEdge::None));
    auto *cnt = c.addComponent(std::make_unique<DllComponent>("cnt", dllFileName("dual_clk_dll")));
    ASSERT_NE(cnt, nullptr);
    auto *rst_c = c.addComponent(constant("rst_c", 0));

    c.connect(ga, "clk", clk_a);
    c.connect(gb, "clk", clk_b);
    c.connect(cnt, "clk_a", clk_a);
    c.connect(cnt, "clk_b", clk_b);
    c.connect(cnt, "rst", rst);
    c.connect(cnt, "count_a", count_a);
    c.connect(cnt, "count_b", count_b);
    c.connect(rst_c, "out", rst);

    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    c.init();

    for (int i = 0; i < 5; i++)
        c.tick();
    EXPECT_EQ(c.getWire(count_a->id())[0], 5u) << "伪时钟每 tick 一个沿";
    EXPECT_EQ(c.getWire(count_b->id())[0], 2u) << "普通时钟只有真实下降沿";
}

TEST(VerilatorPseudoClock, CounterRunsOnEveryTick) {
    Circuit c;
    Net *clk = c.createNet("clk");
    Net *rst = c.createNet("rst");
    Net *en = c.createNet("en");
    Net *count = c.createNet("count");

    auto *gen = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::Rising));
    auto *cnt = c.addComponent(std::make_unique<DllComponent>("cnt", dllFileName("counter8_dll")));
    ASSERT_NE(cnt, nullptr);
    auto *rst_c = c.addComponent(constant("rst_c", 0));
    auto *en_c = c.addComponent(constant("en_c", 1));

    c.connect(gen, "clk", clk);
    c.connect(cnt, "clk", clk);
    c.connect(cnt, "rst", rst);
    c.connect(cnt, "en", en);
    c.connect(cnt, "count", count);
    c.connect(rst_c, "out", rst);
    c.connect(en_c, "out", en);

    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    c.init();

    for (int i = 0; i < 5; i++)
        c.tick();
    EXPECT_EQ(c.getWire(count->id())[0], 5)
            << "伪时钟每个 tick 都应让 Verilator 模型看到一次上升沿";
    EXPECT_EQ(c.getWire(clk->id())[0], 0) << "伪时钟线没有电平语义";
}
