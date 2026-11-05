// 导出 JSON 只嵌入本电路用到的蓝图定义
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <format>
#include <nlohmann/json.hpp>
#include <string>

using namespace dsc;

namespace {

const char *kNotDef = R"JSON({
  "nets": ["a", "y"],
  "components": [
    {"type": "not", "name": "n0", "params": {"bit_width": "1"},
     "connections": {"in": "a", "out": "y"}}
  ],
  "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
})JSON";

// 只含一个输入输出线网的电路，元件引用给定定义
std::string circuitUsing(const std::string &def) {
    return std::format(R"JSON({{
  "definitions": {{ "{0}": {1}, "other_def": {1} }},
  "nets": ["in", "out"],
  "components": [
    {{"type": "composite", "name": "u0", "params": {{"definition": "{0}"}},
      "connections": {{"a": "in", "y": "out"}}}}
  ]
}})JSON",
                       def, kNotDef);
}

nlohmann::json loadAndExport(Context &ctx, const std::string &json) {
    std::string error;
    auto c = Circuit::fromJson(json, error, "", &ctx);
    EXPECT_TRUE(c) << error;
    return nlohmann::json::parse(c ? c->exportJson() : std::string("{}"));
}

} // namespace

// 同一个 Context 的库里可能装着别的电路的定义，导出时不能一起写出去
TEST(CircuitExport, OnlyUsedDefinitions) {
    Context ctx;
    auto root = loadAndExport(ctx, circuitUsing("used_def"));
    ASSERT_TRUE(root.contains("definitions"));
    EXPECT_TRUE(root["definitions"].contains("used_def"));
    EXPECT_FALSE(root["definitions"].contains("other_def"));
}

// 展开之后复合元件已经不在 components 里，靠展开时记录的定义名照样能导出
TEST(CircuitExport, OnlyUsedDefinitionsAfterCompile) {
    Context ctx;
    std::string error;
    auto c = Circuit::fromJson(circuitUsing("used_def"), error, "", &ctx);
    ASSERT_TRUE(c) << error;
    ASSERT_EQ(c->compile().code, ErrorCode::OK);
    EXPECT_GT(c->usedDefinitions().count("used_def"), 0u);
    auto root = nlohmann::json::parse(c->exportJson());
    ASSERT_TRUE(root.contains("definitions"));
    EXPECT_TRUE(root["definitions"].contains("used_def"));
    EXPECT_FALSE(root["definitions"].contains("other_def"));
}

// 蓝图嵌套引用别的蓝图：被引用的定义也要一起导出，否则换台机器展开会找不到
TEST(CircuitExport, NestedDefinitionsIncluded) {
    const char *outer = R"JSON({
  "nets": ["a", "y"],
  "components": [
    {"type": "composite", "name": "c0", "params": {"definition": "inner_def"},
     "connections": {"a": "a", "y": "y"}}
  ],
  "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
})JSON";
    Context ctx;
    std::string error;
    const std::string json = std::format(R"JSON({{
  "definitions": {{ "outer_def": {0}, "inner_def": {1} }},
  "nets": ["in", "out"],
  "components": [
    {{"type": "composite", "name": "u0", "params": {{"definition": "outer_def"}},
      "connections": {{"a": "in", "y": "out"}}}}
  ]
}})JSON",
                                         outer, kNotDef);
    auto c = Circuit::fromJson(json, error, "", &ctx);
    ASSERT_TRUE(c) << error;
    auto root = nlohmann::json::parse(c->exportJson());
    ASSERT_TRUE(root.contains("definitions"));
    EXPECT_TRUE(root["definitions"].contains("outer_def"));
    EXPECT_TRUE(root["definitions"].contains("inner_def"));
}
