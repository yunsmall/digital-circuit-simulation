// Context 隔离、--check 校验、schema 生成和路径列表解析
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/CircuitCheck.h>
#include <dcs/Context.h>
#include <dcs/SchemaGen.h>
#include <dcs/util/PathList.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace dsc;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path root;

    TempDir() {
        static std::atomic<int> seq{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / std::format("dcs-check-{}-{}", stamp, seq++);
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const std::string &rel, const std::string &text) const {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
    }
    std::string base() const {
        return root.string();
    }
};

// 单取反定义（1 个元件）
const char *kNotDef = R"JSON({
  "nets": ["a", "y"],
  "components": [{"type": "not", "name": "n0", "params": {"bit_width": "1"},
                  "connections": {"in": "a", "out": "y"}}],
  "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
})JSON";

// 两级取反（2 个元件）
const char *kDoubleNotDef = R"JSON({
  "nets": ["a", "b", "y"],
  "components": [
    {"type": "not", "name": "n0", "params": {"bit_width": "1"},
     "connections": {"in": "a", "out": "b"}},
    {"type": "not", "name": "n1", "params": {"bit_width": "1"},
     "connections": {"in": "b", "out": "y"}}
  ],
  "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
})JSON";

std::string topJson(const std::string &def_name) {
    // 定义已经导进上下文了，这里只放实例；占位符替换避免和原始字符串混用
    std::string j = R"JSON({"nets": ["a", "y"],
      "components": [{"type": "composite", "name": "u0", "params": {"definition": "__DEF__"},
                      "connections": {"a": "a", "y": "y"}}]})JSON";
    for (size_t pos = 0; (pos = j.find("__DEF__", pos)) != std::string::npos; pos += def_name.size())
        j.replace(pos, 7, def_name);
    return j;
}

} // namespace

// 两个上下文可以有同名蓝图，展开时各查各的库，互不影响
TEST(ContextIsolation, SameDefinitionNameDifferentLibraries) {
    Context ctx_one, ctx_two;
    std::string error;
    auto import = [&](Context &ctx, const char *def) {
        const std::string json = std::string(R"JSON({"definitions": {"ctx_shared": )JSON") + def +
                                 R"JSON(}, "components": []})JSON";
        return Circuit::fromJson(json, error, "", &ctx);
    };
    ASSERT_NE(import(ctx_one, kNotDef), nullptr) << error;
    ASSERT_NE(import(ctx_two, kDoubleNotDef), nullptr) << error;

    auto make = [&](Context &ctx) {
        return Circuit::fromJson(topJson("ctx_shared"), error, "", &ctx);
    };
    auto c1 = make(ctx_one);
    auto c2 = make(ctx_two);
    ASSERT_NE(c1, nullptr) << error;
    ASSERT_NE(c2, nullptr) << error;
    ASSERT_EQ(c1->compile().code, ErrorCode::OK);
    ASSERT_EQ(c2->compile().code, ErrorCode::OK);
    EXPECT_EQ(c1->components().size(), 1u) << "上下文一的蓝图只有一个 not";
    EXPECT_EQ(c2->components().size(), 2u) << "上下文二的蓝图有两个 not";
    // 进程默认上下文不应被这两个自定义上下文污染
    EXPECT_EQ(Context::global().library().find("ctx_shared"), nullptr);
}

// 电路校验：展开复合元件后统计线网和元件，不编译
TEST(CheckTool, CheckCircuitReportsExpandedCounts) {
    TempDir dir;
    dir.write("libs/one.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_not": )JSON") + kNotDef + "}}");
    dir.write("top.json", R"JSON({
      "$meta": {"kind": "circuit", "format": 1},
      "definitions": {"chk_not": "libs/one.json"},
      "nets": ["a", "y"],
      "components": [{"type": "composite", "name": "u0", "params": {"definition": "chk_not"},
                       "connections": {"a": "a", "y": "y"}}]
    })JSON");
    Context ctx;
    CheckReport report;
    std::string error;
    ASSERT_TRUE(checkCircuitFile((dir.root / "top.json").string(), &ctx, report, error)) << error;
    EXPECT_EQ(report.nets, 2);
    EXPECT_EQ(report.components, 1);
    EXPECT_FALSE(report.definitions.empty());
    EXPECT_NE(report.definitions[0].find("chk_not"), std::string::npos);
}

// 蓝图校验：每个定义都实例化一次；未知元件类型要报出来
TEST(CheckTool, CheckBlueprintRejectsUnknownComponent) {
    TempDir dir;
    dir.write("bad.json", R"JSON({
      "$meta": {"kind": "blueprint", "format": 1, "name": "bad_bp"},
      "nets": ["a", "y"],
      "components": [{"type": "no_such_type", "name": "x", "params": {},
                      "connections": {"in": "a", "out": "y"}}],
      "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
    })JSON");
    Context ctx;
    CheckReport report;
    std::string error;
    EXPECT_FALSE(checkBlueprintFile((dir.root / "bad.json").string(), &ctx, report, error));
    EXPECT_NE(error.find("no_such_type"), std::string::npos) << error;
}

// 蓝图文件只引用别的蓝图时，也要把被引用的定义校验一遍
TEST(CheckTool, CheckBlueprintValidatesReferencedDefinitions) {
    TempDir dir;
    dir.write("leaf.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_leaf": )JSON") + kNotDef + "}}");
    dir.write("wrapper.json", R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_leaf": "leaf.json"}
    })JSON");
    Context ctx;
    CheckReport report;
    std::string error;
    ASSERT_TRUE(checkBlueprintFile((dir.root / "wrapper.json").string(), &ctx, report, error)) << error;
    EXPECT_EQ(report.components, 1) << "被引用的蓝图应被实例化检查";
}

// $meta.kind 的默认值：没有 $meta 按 circuit
TEST(CheckTool, ReadJsonKindDefaultsToCircuit) {
    TempDir dir;
    dir.write("legacy.json", R"JSON({"nets": ["a"], "components": []})JSON");
    dir.write("bp.json", R"JSON({"$meta": {"kind": "blueprint"}, "definitions": {}})JSON");
    std::string kind, error;
    ASSERT_TRUE(readJsonKind((dir.root / "legacy.json").string(), kind, error)) << error;
    EXPECT_EQ(kind, "circuit");
    ASSERT_TRUE(readJsonKind((dir.root / "bp.json").string(), kind, error)) << error;
    EXPECT_EQ(kind, "blueprint");
}

// schema 由注册表生成，能解析且带上已注册类型
TEST(SchemaGen, GeneratesParseableSchema) {
    const auto text = componentSchemaJson();
    auto j = nlohmann::json::parse(text);
    ASSERT_TRUE(j.contains("oneOf"));
    bool found_not = false;
    for (auto &variant: j["oneOf"]) {
        if (variant["properties"]["type"]["const"] == "not") {
            found_not = true;
            ASSERT_TRUE(variant["properties"]["params"]["properties"].contains("bit_width"));
        }
    }
    EXPECT_TRUE(found_not) << "schema 里应包含 not 元件";
}

// PATH 风格列表：平台分隔符、空段忽略
TEST(PathList, SplitsWithPlatformSeparator) {
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    const auto parts = splitPathList(std::string("a") + sep + "b" + sep + sep + "c" + sep);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
    EXPECT_TRUE(splitPathList("").empty());
}
