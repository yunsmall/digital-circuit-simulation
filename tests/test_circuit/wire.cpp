// 线网位宽测试
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <gtest/gtest.h>

TEST(WireTest, BitWidthDeterminedByMaxPin) {
    dsc::Circuit c;
    auto *w = c.createNet("w"), *in = c.createNet("in");
    auto *g4 = c.addComponent(std::make_unique<dsc::UnaryGate>("b4", 4));
    auto *g8 = c.addComponent(std::make_unique<dsc::UnaryGate>("b8", 8));
    c.connect(g4, "in", in);
    c.connect(g4, "out", w);
    EXPECT_EQ(w->bit_width(), 4);
    c.connect(g8, "in", w);
    EXPECT_EQ(w->bit_width(), 8);
}

// JSON 用线网名记录连接，同名线网必须自动改名，否则往返后连接会错位
TEST(WireTest, DuplicateNamesBecomeUniqueAndRoundTrip) {
    dsc::Circuit c;
    auto *a1 = c.createNet("a");
    auto *a2 = c.createNet("a");
    EXPECT_EQ(a1->name(), "a");
    EXPECT_EQ(a2->name(), "a_2");
    auto *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", a1);
    c.connect(g, "out", out);

    std::string error;
    auto c2 = dsc::Circuit::fromJson(c.exportJson(), error);
    ASSERT_TRUE(c2 != nullptr) << error;
    ASSERT_TRUE(c2->findNet("a") != nullptr);
    ASSERT_TRUE(c2->findNet("a_2") != nullptr);
    ASSERT_EQ(c2->components().size(), 1u);
    ASSERT_FALSE(c2->components()[0]->inputs().empty());
    EXPECT_EQ(c2->components()[0]->inputs()[0]->net()->name(), "a")
            << "连接必须指向第一次创建的同名线网";
}

// 线网 id 直接作为 JIT 的 _w[] 下标：越界调用不能写坏相邻线网或读越界
TEST(WireTest, InvalidNetIdsDoNotTouchMemory) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in0);
    c.connect(g, "out", out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.setWire(in0->id(), (uint64_t) 0xFF);
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);

    c.setWire(-2, (uint64_t) 0xAA);
    c.setWire(c.netCount(), (uint64_t) 0xAA);
    c.tickCycle(-1);
    c.tickCycle(c.netCount());
    c.tickCycles(-1, 4);
    c.tickCycles(c.netCount(), 4);

    EXPECT_EQ(c.getWire(in0->id())[0], 0xFF) << "越界 id 不能写到别的线网";
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
    for (auto v: c.getWire(-1))
        EXPECT_EQ(v, 0);
    for (auto v: c.getWire(c.netCount()))
        EXPECT_EQ(v, 0);
}

// findNet() 找不到时返回空指针，直接传给 setWire 不能崩（和其他无效输入一致）
TEST(WireTest, NullNetPointerIsIgnored) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in0);
    c.connect(g, "out", out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    dsc::Net *missing = c.findNet("nope");
    ASSERT_EQ(missing, nullptr);
    c.setWire(missing, (uint64_t) 1);
    c.setWire(missing, std::vector<uint8_t>{1});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
}
