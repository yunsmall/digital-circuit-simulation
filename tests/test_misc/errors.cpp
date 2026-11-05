// 错误处理测试：环路检测、多驱动、三态混合
#include <dcs/Circuit.h>
#include <dcs/JitBackend.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

TEST(ErrorTest, CombinationalLoop) {
    dsc::Circuit c;
    auto *w0 = c.createNet("w0"), *w1 = c.createNet("w1");
    auto *g1 = c.addComponent(std::make_unique<dsc::UnaryGate>("g1", 8));
    c.connect(g1, "in", w0);
    c.connect(g1, "out", w1);
    auto *g2 = c.addComponent(std::make_unique<dsc::UnaryGate>("g2", 8));
    c.connect(g2, "in", w1);
    c.connect(g2, "out", w0);
    auto err = c.compile();
    EXPECT_EQ(err.code, dsc::ErrorCode::COMBINATIONAL_LOOP);
    EXPECT_TRUE(err.message.find("环路") != std::string::npos);
}

TEST(ErrorTest, CompileReturnsOK) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 8));
    c.connect(g, "in", in);
    c.connect(g, "out", out);
    EXPECT_EQ(c.compile().code, dsc::ErrorCode::OK);
    EXPECT_TRUE(c.isCompiled());
}

TEST(ErrorTest, PinNotFoundInConnect) {
    dsc::Circuit c;
    auto *net = c.createNet("n");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 8));
    EXPECT_EQ(c.connect(g, "no_such_pin", net).code, dsc::ErrorCode::PIN_NOT_FOUND);
}

TEST(ErrorTest, SequentialBreaksLoop) {
    dsc::Circuit c;
    auto *w0 = c.createNet("w0"), *w1 = c.createNet("w1"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *dff = c.addComponent(std::make_unique<dsc::DFlipFlop>("dff", 8));
    c.connect(dff, "d", w0);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", w1);
    auto *not_g = c.addComponent(std::make_unique<dsc::UnaryGate>("not", 8));
    c.connect(not_g, "in", w1);
    c.connect(not_g, "out", w0);
    EXPECT_EQ(c.compile().code, dsc::ErrorCode::OK);
}

TEST(ErrorTest, MultipleDrivers) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g1 = c.addComponent(std::make_unique<dsc::UnaryGate>("g1", 8));
    c.connect(g1, "in", c.createNet("a"));
    c.connect(g1, "out", w);
    auto *g2 = c.addComponent(std::make_unique<dsc::UnaryGate>("g2", 8));
    c.connect(g2, "in", c.createNet("b"));
    EXPECT_EQ(c.connect(g2, "out", w).code, dsc::ErrorCode::MULTIPLE_DRIVERS);
}

TEST(ErrorTest, TriStateAndNormalOnSameNet) {
    dsc::Circuit c;
    auto *bus = c.createNet("bus");
    auto *ts = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts", 8));
    c.connect(ts, "in", c.createNet("ina"));
    c.connect(ts, "oe", c.createNet("oea"));
    c.connect(ts, "out", bus);
    auto *buf = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8));
    c.connect(buf, "in", c.createNet("inb"));
    EXPECT_EQ(c.connect(buf, "out", bus).code, dsc::ErrorCode::MULTIPLE_DRIVERS);
}

// 绕过 connect() 直接改连接关系（反序列化、以后的编辑接口可能这么干）时，
// check()/compile() 必须在生成代码前把非三态多驱动查出来——
// 直写 _w[] 的优化元件（CPU、WishboneMemory）依赖单驱动前提，不能只是假设
TEST(ErrorTest, BypassedMultiDriverRejectedAtCompile) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g1 = c.addComponent(std::make_unique<dsc::UnaryGate>("g1", 8));
    auto *g2 = c.addComponent(std::make_unique<dsc::UnaryGate>("g2", 8));
    c.connect(g1, "in", c.createNet("a"));
    c.connect(g2, "in", c.createNet("b"));
    c.connect(g1, "out", w);
    g2->outputs()[0]->setNet(w); // 绕过 connect 的检查

    auto err = c.check();
    EXPECT_EQ(err.code, dsc::ErrorCode::MULTIPLE_DRIVERS);
    EXPECT_EQ(err.net_id, w->id());
    EXPECT_EQ(c.compile().code, dsc::ErrorCode::MULTIPLE_DRIVERS);
}

// 生成的 C 编译失败时，编译器诊断必须进入 CircuitError，
// 否则生成器拼错 C 只能看到一句"编译失败"，无从定位
TEST(ErrorTest, JitCompileFailureIncludesDiagnostic) {
    dsc::JitBackend backend;
    auto r = backend.compile("int broken = ;\n", "", {});
    EXPECT_EQ(r.code, dsc::ErrorCode::JIT_COMPILE_FAILED);
    EXPECT_NE(r.message.find("error"), std::string::npos) << r.message;
}
