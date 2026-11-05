// 错误处理测试
#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <dcs/components/combinational/Gates.h>
#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
// 模拟宿主 DLL 的 deinit 回调抛异常
class ThrowingDeinit : public dsc::CombinationalComponent {
public:
    explicit ThrowingDeinit(const std::string &name) : CombinationalComponent(name, "throwing_deinit") {
        addOutput("y", 1);
    }
    std::string genFuncDef_comb() const override {
        return std::format("static void {}() {{\n    const uint8_t _o = 0;\n{}\n}}\n", funcName_comb(),
                           genOutputWrite(0, "_o", 1));
    }
    std::unique_ptr<dsc::Component> clone(const std::string &n) const override {
        return std::make_unique<ThrowingDeinit>(n);
    }
    void simDeinit() override {
        throw std::runtime_error("deinit boom");
    }
};

// 记录 simDeinit 是否被调用：验证一个元件抛异常不会跳过其它元件的清理
class CleanupProbe : public dsc::CombinationalComponent {
public:
    CleanupProbe(const std::string &name, bool *flag) :
        CombinationalComponent(name, "cleanup_probe"), _flag(flag) {
        addOutput("y", 1);
    }
    std::string genFuncDef_comb() const override {
        return std::format("static void {}() {{\n    const uint8_t _o = 0;\n{}\n}}\n", funcName_comb(),
                           genOutputWrite(0, "_o", 1));
    }
    std::unique_ptr<dsc::Component> clone(const std::string &n) const override {
        return std::make_unique<CleanupProbe>(n, _flag);
    }
    void simDeinit() override {
        *_flag = true;
    }

private:
    bool *_flag;
};
} // namespace

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

// deinit 的宿主回调抛异常时：状态必须先停、compile 返回错误而不是 terminate
TEST(ErrorTest, CompileSurvivesThrowingDeinit) {
    dsc::Circuit c;
    auto *out = c.createNet("out");
    auto *bomb = c.addComponent(std::make_unique<ThrowingDeinit>("bomb"));
    c.connect(bomb, "y", out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    ASSERT_TRUE(c.isCompiled());

    auto err = c.compile();
    EXPECT_EQ(err.code, dsc::ErrorCode::PREPARE_FAILED);
    EXPECT_NE(err.message.find("销毁"), std::string::npos) << err.message;
    EXPECT_FALSE(c.isCompiled());
    c.tick(); // 不能继续执行已部分销毁的 JIT
}

// 一个元件的 deinit 回调抛异常不能跳过其它元件的清理
TEST(ErrorTest, DeinitRunsAllSimDeinitWhenOneThrows) {
    bool cleaned = false;
    dsc::Circuit c;
    auto *out = c.createNet("out");
    auto *bomb = c.addComponent(std::make_unique<ThrowingDeinit>("bomb"));
    c.addComponent(std::make_unique<CleanupProbe>("probe", &cleaned));
    c.connect(bomb, "y", out);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    cleaned = false;

    auto err = c.compile(); // 第二次 compile 先 deinit，bomb 在这里抛
    EXPECT_EQ(err.code, dsc::ErrorCode::PREPARE_FAILED);
    EXPECT_TRUE(cleaned) << "bomb 抛异常后 probe 的 simDeinit 仍必须执行";
}
