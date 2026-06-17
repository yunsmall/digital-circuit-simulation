// 错误处理测试：环路检测、多驱动、三态混合
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

TEST(ErrorTest, CombinationalLoop) {
    dsc::Circuit c;
    auto *w0 = c.createNet("w0"), *w1 = c.createNet("w1");
    auto *g1 = c.addComponent(std::make_unique<dsc::GateNOT>("g1", 8));
    c.connect(g1, "in", w0);
    c.connect(g1, "out", w1);
    auto *g2 = c.addComponent(std::make_unique<dsc::GateNOT>("g2", 8));
    c.connect(g2, "in", w1);
    c.connect(g2, "out", w0);
    auto err = c.compile();
    EXPECT_EQ(err.code, dsc::ErrorCode::COMBINATIONAL_LOOP);
    EXPECT_TRUE(err.message.find("环路") != std::string::npos);
}

TEST(ErrorTest, CompileReturnsOK) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateBUF>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    EXPECT_EQ(c.compile().code, dsc::ErrorCode::OK);
    EXPECT_TRUE(c.isCompiled());
}

TEST(ErrorTest, PinNotFoundInConnect) {
    dsc::Circuit c;
    auto *net = c.createNet("n");
    auto *g = c.addComponent(std::make_unique<dsc::GateBUF>("g", 8));
    EXPECT_THROW(c.connect(g, "no_such_pin", net), std::runtime_error);
}

TEST(ErrorTest, SequentialBreaksLoop) {
    dsc::Circuit c;
    auto *w0 = c.createNet("w0"), *w1 = c.createNet("w1"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *dff = c.addComponent(std::make_unique<dsc::DFlipFlop>("dff", 8));
    c.connect(dff, "d", w0);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", w1);
    auto *not_g = c.addComponent(std::make_unique<dsc::GateNOT>("not", 8));
    c.connect(not_g, "in", w1);
    c.connect(not_g, "out", w0);
    EXPECT_EQ(c.compile().code, dsc::ErrorCode::OK);
}

TEST(ErrorTest, MultipleDrivers) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g1 = c.addComponent(std::make_unique<dsc::GateBUF>("g1", 8));
    c.connect(g1, "in", c.createNet("a"));
    c.connect(g1, "out", w);
    auto *g2 = c.addComponent(std::make_unique<dsc::GateBUF>("g2", 8));
    c.connect(g2, "in", c.createNet("b"));
    EXPECT_THROW(c.connect(g2, "out", w), std::runtime_error);
}

TEST(ErrorTest, TriStateAndNormalOnSameNet) {
    dsc::Circuit c;
    auto *bus = c.createNet("bus");
    auto *ts = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts", 8));
    c.connect(ts, "in", c.createNet("ina"));
    c.connect(ts, "oe", c.createNet("oea"));
    c.connect(ts, "out", bus);
    auto *buf = c.addComponent(std::make_unique<dsc::GateBUF>("buf", 8));
    c.connect(buf, "in", c.createNet("inb"));
    EXPECT_THROW(c.connect(buf, "out", bus), std::runtime_error);
}
