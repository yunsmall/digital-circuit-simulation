// 错误处理测试
#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <dcs/components/combinational/Gates.h>
#include <gtest/gtest.h>

TEST(ErrorTest, UnconnectedPinIsZero) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in0);
    c.connect(g, "out", out);
    c.compile();
    c.init();
    c.setWire(in0->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x00);
}

TEST(ErrorTest, InvalidGateInputs) {
    EXPECT_THROW(dsc::LogicGate("g", 0, 8, dsc::GateOp::AND), std::invalid_argument);
    EXPECT_THROW(dsc::LogicGate("g", 9, 8, dsc::GateOp::AND), std::invalid_argument);
    EXPECT_THROW(dsc::LogicGate("g", 2, 129, dsc::GateOp::AND), std::invalid_argument);
}

// connect 必须先拒绝空指针和外来线网，否则后面按 net->id() 写 _bus_nets/_w[]
// 会踩到别的电路或直接解引用空指针
TEST(ErrorTest, ConnectRejectsNullAndForeignNet) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));

    EXPECT_EQ(c.connect(g, "in0", nullptr).code, dsc::ErrorCode::INVALID_NET);
    dsc::Circuit other;
    auto *foreign = other.createNet("foreign");
    EXPECT_EQ(c.connect(g, "in0", foreign).code, dsc::ErrorCode::INVALID_NET);
    EXPECT_EQ(g->inputs()[0]->net(), nullptr) << "失败时不能留下半条连接";

    EXPECT_EQ(c.connect(nullptr, "in0", in0).code, dsc::ErrorCode::PIN_NOT_FOUND);
    EXPECT_EQ(c.connect(g, "in0", in0).code, dsc::ErrorCode::OK);
    EXPECT_EQ(c.connect(g, "out", out).code, dsc::ErrorCode::OK);
}

// 连接关系按名字序列化，同名线网导入后会静默指向最后一个，必须拒绝
TEST(ErrorTest, DuplicateNetNamesInJsonRejected) {
    std::string error;
    auto c = dsc::Circuit::fromJson(R"({"nets":["a","a"],"components":[]})", error);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(error.find("重复"), std::string::npos) << error;
}

// compile 后新增线网必须先让旧 JIT 失效：旧 _w[] 没有这个 id，
// setWire 的边界检查却按新的 _nets.size() 放行，会越界写
TEST(ErrorTest, CreateNetInvalidatesCompiled) {
    dsc::Circuit c;
    auto *in0 = c.createNet("in0"), *out = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 8, dsc::GateOp::AND));
    c.connect(g, "in0", in0);
    c.connect(g, "in1", in0);
    c.connect(g, "out", out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    ASSERT_TRUE(c.isCompiled());

    auto *extra = c.createNet("extra");
    EXPECT_FALSE(c.isCompiled()) << "新增线网后旧 JIT 不再对应当前网表";
    c.setWire(extra->id(), 1); // 必须被 _compiled 拦住，不能写穿旧 JIT 的 _w[]
}

// 动态引脚在 ensurePins 时已按旧上下文解析，换库会让引脚和展开用的定义对不上
TEST(ErrorTest, SetContextRejectedAfterComponents) {
    dsc::Circuit c;
    c.addComponent(std::make_unique<dsc::LogicGate>("g", 2, 1, dsc::GateOp::AND));
    dsc::Context ctx;
    EXPECT_THROW(c.setContext(ctx), std::logic_error);
}
