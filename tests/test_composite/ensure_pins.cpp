// ensurePins：引脚描述来自外部资源的元件（Composite、DLL）在构造期拿不到引脚，
// 必须由 ensurePins 补齐；工具可以只调 ensurePins，电路加载则经 prepare 自动调用
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <dcs/components/memory/Memory.h>
#include <dcs/components/platform/Composite.h>

#include "common.h"

using namespace dsc;

namespace {
// 位宽来自蓝图参数的组合元件，用来验证 ensurePins 的参数覆盖
void regWidthAnd() {
    if (CircuitLibrary::instance().exists("width_and"))
        return;
    CircuitDefinition def;
    def.name = "width_and";
    def.params = {{"w", "8"}};
    def.nets = {"a", "b", "y"};
    def.components = {
            {"and", "g", {{"inputs", "2"}, {"bit_width", "${w}"}}, {{"in0", "a"}, {"in1", "b"}, {"out", "y"}}},
    };
    def.input_expose = {{"a", {"a", "${w}"}}, {"b", {"b", "${w}"}}};
    def.output_expose = {{"y", {"y", "${w}"}}};
    CircuitLibrary::instance().add(std::move(def));
}
} // namespace

TEST(EnsurePins, PrepareBuildsCompositePins) {
    regHalfAdder();
    Context &ctx = Context::global();

    CompositeComponent c("ha", "half_adder");
    EXPECT_TRUE(c.inputs().empty()) << "构造期还没有蓝图，引脚为空";
    EXPECT_TRUE(c.outputs().empty());

    std::string err;
    ASSERT_TRUE(c.prepare(ctx, err)) << err;
    EXPECT_TRUE(c.isPrepared());
    EXPECT_EQ(c.inputs().size(), 2u);
    EXPECT_EQ(c.outputs().size(), 2u);
}

TEST(EnsurePins, Idempotent) {
    regHalfAdder();
    Context &ctx = Context::global();

    CompositeComponent c("ha", "half_adder");
    std::string err;
    ASSERT_TRUE(c.ensurePins(ctx, err)) << err;
    const size_t ins = c.inputs().size(), outs = c.outputs().size();
    ASSERT_TRUE(c.ensurePins(ctx, err)) << err;
    EXPECT_EQ(c.inputs().size(), ins) << "重复调用不能重复建引脚";
    EXPECT_EQ(c.outputs().size(), outs);

    // prepare 内部也会调 ensurePins，同样不能叠加
    ASSERT_TRUE(c.prepare(ctx, err)) << err;
    EXPECT_EQ(c.inputs().size(), ins);
    EXPECT_EQ(c.outputs().size(), outs);
}

// 工具（--pins、GUI 元件面板）只想要引脚，不需要分配存储或读镜像
TEST(EnsurePins, AloneIsEnoughForTools) {
    regHalfAdder();
    Context &ctx = Context::global();

    CompositeComponent c("ha", "half_adder");
    std::string err;
    ASSERT_TRUE(c.ensurePins(ctx, err)) << err;
    EXPECT_EQ(c.inputs().size(), 2u);
    EXPECT_FALSE(c.isPrepared()) << "ensurePins 不负责运行期准备";
}

TEST(EnsurePins, WidthExpressionUsesInstanceParams) {
    regWidthAnd();
    Context &ctx = Context::global();

    CompositeComponent c("g", "width_and", {{"w", "16"}});
    std::string err;
    ASSERT_TRUE(c.ensurePins(ctx, err)) << err;
    ASSERT_EQ(c.inputs().size(), 2u);
    ASSERT_EQ(c.outputs().size(), 1u);
    EXPECT_EQ(c.inputs()[0]->bit_width(), 16);
    EXPECT_EQ(c.inputs()[1]->bit_width(), 16);
    EXPECT_EQ(c.outputs()[0]->bit_width(), 16);
}

TEST(EnsurePins, MissingDefinitionFailsWithoutPins) {
    Context &ctx = Context::global();
    CompositeComponent c("x", "no_such_blueprint_xyz");
    std::string err;
    EXPECT_FALSE(c.ensurePins(ctx, err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(c.inputs().empty());
    EXPECT_TRUE(c.outputs().empty());
}

TEST(EnsurePins, BadParamFailsWithoutPartialPins) {
    regWidthAnd();
    Context &ctx = Context::global();
    CompositeComponent c("g", "width_and", {{"nope", "1"}});
    std::string err;
    EXPECT_FALSE(c.ensurePins(ctx, err));
    EXPECT_NE(err.find("nope"), std::string::npos);
    EXPECT_TRUE(c.inputs().empty()) << "失败不能留下半套引脚";
    EXPECT_TRUE(c.outputs().empty());
}

// 默认实现是空操作：普通元件构造期就建好引脚，ensurePins 不能顺手做重活
TEST(EnsurePins, DefaultIsNoOp) {
    Context &ctx = Context::global();
    Memory m("m", 4, 8);
    const size_t ins = m.inputs().size(), outs = m.outputs().size();
    std::string err;
    EXPECT_TRUE(m.ensurePins(ctx, err));
    EXPECT_EQ(m.inputs().size(), ins);
    EXPECT_EQ(m.outputs().size(), outs);
    EXPECT_FALSE(m.isPrepared());
    EXPECT_EQ(m.writableMemPtr(), nullptr) << "ensurePins 不能分配存储";
}
