// 顶层电路 JSON 与蓝图 JSON 的两种文件形式。
//
// 顶层文件（$meta.kind = "circuit"，可省略）的 definitions 里，值可以是内联定义，
// 也可以是蓝图文件路径；相对路径先按引用文件所在目录找，再按 --lib-path 的顺序找。
// 蓝图文件（$meta.kind = "blueprint"）不能直接当电路加载，只能被引用。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/CircuitLibrary.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

using namespace dsc;

namespace {

namespace fs = std::filesystem;

// 每个用例一个独立临时目录，避免文件名和定义名互相干扰
struct TempDir {
    fs::path root;

    TempDir() {
        static std::atomic<int> seq{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / std::format("dcs-json-{}-{}", stamp, seq++);
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
        CircuitLibrary::instance().clearSearchPaths();
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

// 一个单输入的取反蓝图（定义体，不含 $meta）
const char *kNotDef = R"JSON({
  "nets": ["a", "y"],
  "components": [
    {"type": "not", "name": "n0", "params": {"bit_width": "1"},
     "connections": {"in": "a", "out": "y"}}
  ],
  "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
})JSON";

struct Loaded {
    std::unique_ptr<Circuit> circuit;
    std::string error;
};

Loaded load(const std::string &json, const std::string &base_dir) {
    std::string error;
    auto c = Circuit::fromJson(json, error, base_dir);
    return {std::move(c), std::move(error)};
}

} // namespace

// definitions 里用相对路径引用蓝图文件；定义先导入，composite 实例能直接展开
TEST(JsonLibraryFiles, RelativeBlueprintPath) {
    TempDir dir;
    dir.write("libs/half.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_not_rel": )JSON") + kNotDef + "}}");

    const std::string top = R"JSON({
      "$meta": {"kind": "circuit", "format": 1},
      "definitions": {"bp_not_rel": "libs/half.json"},
      "nets": ["a", "y"],
      "components": [
        {"type": "composite", "name": "u0", "params": {"definition": "bp_not_rel"},
         "connections": {"a": "a", "y": "y"}}
      ]
    })JSON";

    auto r = load(top, dir.base());
    ASSERT_NE(r.circuit, nullptr) << r.error;
    EXPECT_TRUE(CircuitLibrary::instance().exists("bp_not_rel"));
    const auto e = r.circuit->compile();
    EXPECT_EQ(e.code, ErrorCode::OK) << e.message;
    EXPECT_EQ(r.circuit->components().size(), 1u) << "复合元件应当被展开成里面的 not";
}

// 单定义蓝图：内容直接写在根上，名字取 $meta.name
TEST(JsonLibraryFiles, SingleDefinitionBlueprintUsesMetaName) {
    TempDir dir;
    dir.write("single.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1, "name": "bp_single_meta"},
      "nets": ["a", "y"],
      "components": [{"type": "not", "name": "n0", "params": {"bit_width": "1"},
                      "connections": {"in": "a", "out": "y"}}],
      "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
    })JSON"));

    const std::string top = R"JSON({
      "definitions": {"whatever": "single.json"},
      "components": []
    })JSON";
    auto r = load(top, dir.base());
    ASSERT_NE(r.circuit, nullptr) << r.error;
    EXPECT_TRUE(CircuitLibrary::instance().exists("bp_single_meta"));
}

// 相对路径找不到时按搜索路径（--lib-path）继续找
TEST(JsonLibraryFiles, SearchPathFallback) {
    TempDir dir;
    dir.write("libs/shared.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_shared_path": )JSON") + kNotDef + "}}");
    dir.write("proj/top.json", R"JSON({
      "definitions": {"bp_shared_path": "shared.json"},
      "components": []
    })JSON");

    CircuitLibrary::instance().addSearchPath((dir.root / "libs").string());
    std::string error;
    auto c = Circuit::fromJson(R"JSON({
      "definitions": {"bp_shared_path": "shared.json"},
      "components": []
    })JSON",
                               error, (dir.root / "proj").string());
    ASSERT_NE(c, nullptr) << error;
    EXPECT_TRUE(CircuitLibrary::instance().exists("bp_shared_path"));
}

// 找不到文件时要把找过的路径说清楚
TEST(JsonLibraryFiles, MissingBlueprintFileReportsPath) {
    TempDir dir;
    auto r = load(R"JSON({
      "definitions": {"x": "nope/absent.json"},
      "components": []
    })JSON",
                  dir.base());
    EXPECT_EQ(r.circuit, nullptr);
    EXPECT_NE(r.error.find("absent.json"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("lib-path"), std::string::npos) << r.error;
}

// 引用的文件没写 kind=blueprint 时拒绝，避免误把电路文件当蓝图
TEST(JsonLibraryFiles, BlueprintFileRequiresKind) {
    TempDir dir;
    dir.write("plain.json", R"JSON({"definitions": {"bp_plain": )JSON" + std::string(kNotDef) + "}}");
    auto r = load(R"JSON({
      "definitions": {"bp_plain": "plain.json"},
      "components": []
    })JSON",
                  dir.base());
    EXPECT_EQ(r.circuit, nullptr);
    EXPECT_NE(r.error.find("blueprint"), std::string::npos) << r.error;
}

// 蓝图文件不能当顶层电路加载
TEST(JsonLibraryFiles, BlueprintCannotBeTopLevel) {
    TempDir dir;
    dir.write("bp.json", std::string(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_top": )JSON") + kNotDef + "}");
    auto r = load(R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_top": )JSON" + std::string(kNotDef) + "}}",
                  dir.base());
    EXPECT_EQ(r.circuit, nullptr);
    EXPECT_NE(r.error.find("蓝图"), std::string::npos) << r.error;
}

// 格式版本比程序新时直接拒绝，而不是猜着加载
TEST(JsonLibraryFiles, RejectsNewerFormatVersion) {
    TempDir dir;
    auto r = load(R"JSON({"$meta": {"kind": "circuit", "format": 999}, "components": []})JSON", dir.base());
    EXPECT_EQ(r.circuit, nullptr);
    EXPECT_NE(r.error.find("999"), std::string::npos) << r.error;
}

// 蓝图互相引用成环时给出明确错误，不能无限递归
TEST(JsonLibraryFiles, DetectsReferenceCycle) {
    TempDir dir;
    dir.write("a.json", R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_cycle_a": "b.json"}
    })JSON");
    dir.write("b.json", R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_cycle_b": "a.json"}
    })JSON");
    auto r = load(R"JSON({
      "definitions": {"bp_cycle_a": "a.json"},
      "components": []
    })JSON",
                  dir.base());
    EXPECT_EQ(r.circuit, nullptr);
    EXPECT_NE(r.error.find("循环"), std::string::npos) << r.error;
}

// 同名时顶层内联定义覆盖文件里的定义
TEST(JsonLibraryFiles, InlineDefinitionOverridesFile) {
    // 文件里的版本只有一个 not；内联版本有两个元件，用来区分谁生效
    const char *inline_def = R"JSON({
      "nets": ["a", "b", "y"],
      "components": [
        {"type": "not", "name": "n0", "params": {"bit_width": "1"},
         "connections": {"in": "a", "out": "b"}},
        {"type": "not", "name": "n1", "params": {"bit_width": "1"},
         "connections": {"in": "b", "out": "y"}}
      ],
      "expose": {"inputs": {"a": "a"}, "outputs": {"y": "y"}}
    })JSON";
    TempDir dir;
    // 文件里同时提供 bp_dup（单元件）和 bp_from_file
    std::string file_defs = R"JSON({
      "$meta": {"kind": "blueprint", "format": 1},
      "definitions": {"bp_dup": )JSON";
    file_defs += kNotDef;
    file_defs += R"JSON(,
                      "bp_from_file": )JSON";
    file_defs += kNotDef;
    file_defs += "}}";
    dir.write("libs/dup.json", file_defs);
    const std::string top = std::string(R"JSON({
      "definitions": {
        "bp_from_file": "libs/dup.json",
        "bp_dup": )JSON") + inline_def + R"JSON(
      },
      "components": []
    })JSON";
    auto r = load(top, dir.base());
    ASSERT_NE(r.circuit, nullptr) << r.error;
    const auto *dup = CircuitLibrary::instance().find("bp_dup");
    ASSERT_NE(dup, nullptr);
    EXPECT_EQ(dup->components.size(), 2u) << "内联定义应当覆盖文件里的同名定义";
    const auto *from_file = CircuitLibrary::instance().find("bp_from_file");
    ASSERT_NE(from_file, nullptr);
}

// 元数据里的作者/说明在 JSON 往返后保留，导出的文件带上 kind 和 format
TEST(JsonLibraryFiles, MetaRoundTrip) {
    TempDir dir;
    auto r = load(R"JSON({
      "$meta": {"kind": "circuit", "format": 1, "author": "测试作者", "description": "往返检查"},
      "components": []
    })JSON",
                  dir.base());
    ASSERT_NE(r.circuit, nullptr) << r.error;

    const auto exported = nlohmann::json::parse(r.circuit->exportJson());
    ASSERT_TRUE(exported.contains("$meta"));
    EXPECT_EQ(exported["$meta"]["kind"], "circuit");
    EXPECT_EQ(exported["$meta"]["format"], 1);
    EXPECT_EQ(exported["$meta"]["author"], "测试作者");

    std::string error;
    auto again = Circuit::fromJson(exported.dump(), error, dir.base());
    ASSERT_NE(again, nullptr) << error;
    EXPECT_EQ(again->author(), "测试作者");
    EXPECT_EQ(again->description(), "往返检查");
}

// 没有 $meta 的老电路文件照常加载（向后兼容）
TEST(JsonLibraryFiles, LegacyCircuitWithoutMeta) {
    TempDir dir;
    auto r = load(R"JSON({"nets": ["a"], "components": [
      {"type": "constant", "name": "c0", "params": {"bit_width": "1", "value": "1"},
       "connections": {"out": "a"}}
    ]})JSON",
                  dir.base());
    ASSERT_NE(r.circuit, nullptr) << r.error;
    EXPECT_EQ(r.circuit->components().size(), 1u);
}
