// 边沿检测测试（时钟采样型）
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

static void doRisingEdge(dsc::Circuit &c, int clk_id) {
    std::vector<uint8_t> v1(1, 1), v0(1, 0);
    c.setWire(clk_id, v1);
    c.tick();
    c.setWire(clk_id, v0);
    c.tick();
}

TEST(EdgeDetectTest, RisingEdge) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::EdgeDetect>("ed", dsc::EdgeType::Rising));
    c.connect(g, "clk", clk);
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();

    std::vector<uint8_t> v1(1, 1), v0(1, 0);

    // in=0, clk↑ → 无上升沿，out=0
    c.setWire(in->id(), v0);
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);

    // in=1, clk↑ → 上升沿 0→1，out=1
    c.setWire(in->id(), v1);
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);

    // in=1, clk↑ → 无新边沿，out=0
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);
}

TEST(EdgeDetectTest, FallingEdge) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::EdgeDetect>("ed", dsc::EdgeType::Falling));
    c.connect(g, "clk", clk);
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();

    std::vector<uint8_t> v1(1, 1), v0(1, 0);

    // in=1, clk↑ → 采样到1，但无下降沿（prev_in初始=0，1→1无变化）
    c.setWire(in->id(), v1);
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);

    // in=0, clk↑ → 下降沿 1→0，out=1
    c.setWire(in->id(), v0);
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}

TEST(EdgeDetectTest, BothEdges) {
    dsc::Circuit c;
    auto *clk = c.createNet("clk"), *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::EdgeDetect>("ed", dsc::EdgeType::Both));
    c.connect(g, "clk", clk);
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    c.compile();
    c.init();

    std::vector<uint8_t> v1(1, 1), v0(1, 0);

    // in 0→1, clk↑ → 上升沿，out=1
    c.setWire(in->id(), v1);
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);

    // 保持 1, clk↑ → 无变化
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 0);

    // in 1→0, clk↑ → 下降沿，out=1
    c.setWire(in->id(), v0);
    doRisingEdge(c, clk->id());
    EXPECT_EQ(c.getWire(out->id())[0] & 1, 1);
}
