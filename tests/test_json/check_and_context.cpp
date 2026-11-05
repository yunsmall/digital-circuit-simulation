// Context 隔离、--check 校验、schema 生成和路径列表解析
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/CircuitCheck.h>
#include <dcs/Context.h>
#include <dcs/SchemaGen.h>
#include <dcs/util/PathList.h>

#include <algorithm>
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

// 被引用的依赖已经先在库里时，仍要按本次导入重新校验：只靠"名字新出现"判断会漏掉它
TEST(CheckTool, CheckReferencedBlueprintAlreadyInLibrary) {
    TempDir dir;
    dir.write("leaf.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_leaf_existing": )JSON") + kNotDef + "}}");
    dir.write("wrapper.json", R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_leaf_existing": "leaf.json"}
    })JSON");
    Context ctx;
    std::string error;
    ASSERT_TRUE(loadDefinitionsFromFile((dir.root / "leaf.json").string(), &ctx, error)) << error;
    CheckReport report;
    ASSERT_TRUE(checkBlueprintFile((dir.root / "wrapper.json").string(), &ctx, report, error)) << error;
    EXPECT_EQ(report.components, 1) << "库中已有的依赖也要重新实例化检查";
}

// 单定义蓝图没有名字，独立校验时用文件名做键；库里已有同名定义时不能覆盖用户定义
TEST(CheckTool, UnnamedBlueprintDoesNotOverwriteExistingDefinition) {
    TempDir dir;
    dir.write("gate.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "nets": ["a", "y"],
      "components": [{"type": "not", "name": "n0", "params": {"bit_width": "1"},
                      "connections": {"in": "a", "out": "y"}}],
      "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
    })JSON"));
    Context ctx;
    std::string error;
    // 用户上下文里先有一个同名的两元件定义
    const std::string user_json = std::string(R"JSON({"definitions": {"gate": )JSON") + kDoubleNotDef +
                                   R"JSON(}, "components": []})JSON";
    ASSERT_NE(Circuit::fromJson(user_json, error, "", &ctx), nullptr) << error;

    CheckReport report;
    ASSERT_TRUE(checkBlueprintFile((dir.root / "gate.json").string(), &ctx, report, error)) << error;
    EXPECT_EQ(report.components, 1) << "校验的是文件里的单定义蓝图";
    const auto *user_def = ctx.library().find("gate");
    ASSERT_NE(user_def, nullptr);
    EXPECT_EQ(user_def->components.size(), 2u) << "用户已有的同名定义不能被覆盖";
}

// 字段类型写错只能报错，不能让异常穿出去把进程带走
TEST(JsonTypes, MalformedFieldsProduceErrors) {
    struct Case {
        const char *json;
        const char *needle;
    };
    const Case cases[] = {
            {R"({"nets": [1], "components": []})", "nets[0]"},
            {R"({"nets": [], "components": [1]})", "components[0]"},
            {R"({"nets": [], "components": [{"type": 3}]})", "type"},
            {R"({"$meta": {"author": 1}, "nets": [], "components": []})", "author"},
            {R"({"$meta": {"kind": 1}, "nets": [], "components": []})", "$meta.kind"},
            {R"({"definitions": {"d": {"nets": [1]}}, "components": []})", "nets"},
    };
    for (const auto &c: cases) {
        std::string error;
        auto circuit = Circuit::fromJson(c.json, error);
        EXPECT_EQ(circuit, nullptr) << c.json;
        EXPECT_NE(error.find(c.needle), std::string::npos) << c.json << ": " << error;
    }
}

// 直接调库接口时，根不是对象、定义字段类型错误都要写 error
TEST(JsonTypes, LibraryImportReportsFieldTypes) {
    CircuitLibrary lib;
    std::string error;
    lib.importJson("[1, 2]", error);
    EXPECT_FALSE(error.empty()) << "根不是对象时必须报错";

    error.clear();
    lib.importJson(R"({"d": {"nets": [1]}})", error);
    EXPECT_NE(error.find("nets"), std::string::npos) << error;
    EXPECT_EQ(lib.find("d"), nullptr) << "导入失败时不能留下半份定义";

    error.clear();
    lib.importJson(R"({"d": {"nets": ["a"], "expose": {"inputs": {"i": 1}}}})", error);
    EXPECT_NE(error.find("expose.inputs"), std::string::npos) << error;
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

// --check 与加载必须用同一套 $meta 校验：format 比程序新时两边都要拒绝，
// 否则检查报告"通过"、加载却报版本不支持
TEST(CheckTool, ReadJsonKindRejectsNewerFormat) {
    TempDir dir;
    dir.write("new.json", R"JSON({"$meta": {"kind": "blueprint", "format": 99}, "definitions": {}})JSON");
    std::string kind, error;
    EXPECT_FALSE(readJsonKind((dir.root / "new.json").string(), kind, error));
    EXPECT_NE(error.find("格式版本"), std::string::npos) << error;
}

// 蓝图校验走 readJsonKind 分流，也必须拒绝新格式（以前只有加载路径看 format）
TEST(CheckTool, CheckBlueprintRejectsNewerFormat) {
    TempDir dir;
    dir.write("new_bp.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 99},
      "definitions": {"fmt_bp": )JSON") + kNotDef + "}}");
    Context ctx;
    CheckReport report;
    std::string error;
    EXPECT_FALSE(checkBlueprintFile((dir.root / "new_bp.json").string(), &ctx, report, error));
    EXPECT_NE(error.find("格式版本"), std::string::npos) << error;
}

// schema 由注册表生成，能解析且带上已注册类型
TEST(SchemaGen, GeneratesParseableSchema) {
    const auto text = componentSchemaJson();
    auto j = nlohmann::json::parse(text);
    ASSERT_TRUE(j.contains("oneOf"));
    bool found_not = false, found_composite = false;
    for (auto &variant: j["oneOf"]) {
        if (variant["properties"]["type"]["const"] == "not") {
            found_not = true;
            ASSERT_TRUE(variant["properties"]["params"]["properties"].contains("bit_width"));
            // 参数在 JSON 里也可以直接写数字（flattenParams 会 dump 成字符串），
            // schema 必须把标量类型也列为合法，否则 IDE 会把能跑的文件标红
            const auto &bw_type = variant["properties"]["params"]["properties"]["bit_width"]["type"];
            ASSERT_TRUE(bw_type.is_array());
            EXPECT_NE(std::find(bw_type.begin(), bw_type.end(), "integer"), bw_type.end());
            EXPECT_NE(std::find(bw_type.begin(), bw_type.end(), "string"), bw_type.end());
            // 和工厂的未知参数校验一致：不允许额外参数的类型，schema 也要标红
            EXPECT_FALSE(variant["properties"]["params"]["additionalProperties"].get<bool>());
        }
        if (variant["properties"]["type"]["const"] == "composite") {
            found_composite = true;
            EXPECT_TRUE(variant["properties"]["params"]["additionalProperties"].get<bool>())
                    << "composite 允许蓝图参数，schema 必须放行额外键";
        }
    }
    EXPECT_TRUE(found_not) << "schema 里应包含 not 元件";
    EXPECT_TRUE(found_composite) << "schema 里应包含 composite 元件";
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

// --list-definitions 只导入 definitions：电路文件没有 components、或者元件建不出来
// （ROM/DLL 路径缺失）都不该影响定义导入
TEST(CheckTool, ListDefinitionsIgnoresCircuitComponents) {
    TempDir dir;
    dir.write("defs_only.json", std::string(R"JSON({
      "definitions": {"chk_list_def": )JSON") + kNotDef + "}}");
    Context ctx;
    std::string error;
    ASSERT_TRUE(loadDefinitionsFromFile((dir.root / "defs_only.json").string(), &ctx, error)) << error;
    EXPECT_NE(ctx.library().find("chk_list_def"), nullptr);
}

// 定义导入失败必须整体回滚：另一个蓝图路径不存在时，前面已导入的定义不能留在库里
TEST(CheckTool, FailedDefinitionImportRollsBackLibrary) {
    TempDir dir;
    dir.write("good.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_rollback_good": )JSON") + kNotDef + "}}");
    const std::string top = R"JSON({
      "definitions": {
        "chk_rollback_good": "good.json",
        "chk_rollback_missing": "no_such_blueprint.json"
      },
      "components": []
    })JSON";
    Context ctx;
    std::string error;
    EXPECT_EQ(Circuit::fromJson(top, error, dir.base(), &ctx), nullptr);
    EXPECT_NE(error.find("no_such_blueprint"), std::string::npos) << error;
    EXPECT_EQ(ctx.library().find("chk_rollback_good"), nullptr) << "失败的导入留下了半个库";
}

// 元件创建失败（未知类型）时，之前导入的定义也要回滚
TEST(CheckTool, ComponentFailureRollsBackLibrary) {
    TempDir dir;
    dir.write("good.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"chk_rollback2": )JSON") + kNotDef + "}}");
    const std::string top = R"JSON({
      "definitions": {"chk_rollback2": "good.json"},
      "components": [{"type": "no_such_type", "name": "bad"}]
    })JSON";
    Context ctx;
    std::string error;
    EXPECT_EQ(Circuit::fromJson(top, error, dir.base(), &ctx), nullptr);
    EXPECT_NE(error.find("no_such_type"), std::string::npos) << error;
    EXPECT_EQ(ctx.library().find("chk_rollback2"), nullptr) << "失败的导入留下了半个库";
}

// 蓝图里的嵌套参数要和顶层电路一样拍平成点分键（backend_opts.hostfwd）
TEST(JsonTypes, BlueprintNestedParamsAreFlattened) {
    Context ctx;
    std::string error;
    const std::string json = R"JSON({
      "definitions": {"chk_flat": {
        "nets": ["a", "y"],
        "components": [{"type": "not", "name": "n0",
          "params": {"bit_width": "1",
                     "backend_opts": {"hostfwd": "tcp::1", "nested": {"k": "v"}}},
          "connections": {"in": "a", "out": "y"}}],
        "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}}},
      "components": []
    })JSON";
    ASSERT_NE(Circuit::fromJson(json, error, "", &ctx), nullptr) << error;
    const auto *def = ctx.library().find("chk_flat");
    ASSERT_NE(def, nullptr);
    ASSERT_EQ(def->components.size(), 1u);
    const auto &p = def->components[0].params;
    EXPECT_EQ(p.count("backend_opts"), 0u) << "嵌套对象没有拍平";
    EXPECT_EQ(p.at("backend_opts.hostfwd"), "tcp::1");
    EXPECT_EQ(p.at("backend_opts.nested.k"), "v");
}

// 容器字段存在但类型不对必须报错，不能当成"没写"静默接受
TEST(JsonTypes, ContainerFieldsMustHaveRightType) {
    struct Case {
        const char *json;
        const char *needle;
    };
    const Case cases[] = {
            {R"({"nets": "foo", "components": []})", "nets 必须是数组"},
            {R"({"nets": [], "components": [{"type": "not", "params": []}]})", "params 必须是对象"},
            {R"({"nets": [], "components": [{"type": "not", "connections": []}]})",
             "connections 必须是对象"},
            {R"({"nets": [], "components": [{"type": "not", "connections": {"in": 3}}]})",
             "connections['in']"},
            {R"({"definitions": {"d": {"nets": ["a"], "components": [{"type": "not", "params": []}]}},
                 "components": []})",
             "params 必须是对象"},
            {R"({"definitions": {"d": {"nets": ["a"], "expose": []}}, "components": []})",
             "expose 必须是对象"},
            {R"({"definitions": {"d": {"nets": ["a"],
                 "expose": {"inputs": {"p": "x"}, "outputs": {"p": "y"}}}}, "components": []})",
             "同时出现在"},
    };
    for (const auto &c: cases) {
        Context ctx;
        std::string error;
        auto circuit = Circuit::fromJson(c.json, error, "", &ctx);
        EXPECT_EQ(circuit, nullptr) << c.json;
        EXPECT_NE(error.find(c.needle), std::string::npos) << c.json << ": " << error;
    }
}
