// 蓝图目录扫描：递归深度、依赖重复导入、跳过非蓝图 JSON、重名/坏文件回滚
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include <dcs/CircuitCheck.h>
#include <dcs/Context.h>
#include <dcs/components/platform/Composite.h>
#include <nlohmann/json.hpp>

using namespace dsc;
namespace fs = std::filesystem;

namespace {

nlohmann::json bufBlueprint(const std::string &name) {
    nlohmann::json comp;
    comp["type"] = "buf";
    comp["name"] = "b";
    comp["params"]["bit_width"] = "1";
    comp["connections"]["in"] = "i";
    comp["connections"]["out"] = "o";

    nlohmann::json j;
    j["$meta"]["kind"] = "blueprint";
    j["$meta"]["format"] = 1;
    j["$meta"]["name"] = name;
    j["nets"] = nlohmann::json::array({"i", "o"});
    j["components"] = nlohmann::json::array({comp});
    j["expose"]["inputs"]["i"] = "i";
    j["expose"]["outputs"]["o"] = "o";
    return j;
}

// 每个测试用独立临时目录，避免并行测试互相踩
struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<unsigned> seq{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / std::format("dcs_lib_test_{}_{}", (long long) stamp, seq++);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const fs::path &rel, const nlohmann::json &j) {
        writeText(rel, j.dump(2));
    }
    void writeText(const fs::path &rel, const std::string &text) {
        const fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream o(p);
        if (!o)
            throw std::runtime_error("无法写入临时文件 " + p.string());
        o << text;
    }
};

} // namespace

TEST(LibraryScan, RootOnlyWhenDepthIsZero) {
    TempDir d;
    d.write("a.json", bufBlueprint("a"));
    d.write("sub/b.json", bufBlueprint("b"));

    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, d.path.string(), 0, imported, error)) << error;
    EXPECT_EQ(imported, 1);
    EXPECT_TRUE(ctx.library().exists("a"));
    EXPECT_FALSE(ctx.library().exists("b")) << "depth=0 不应进入子目录";
}

TEST(LibraryScan, DepthOneIncludesImmediateSubdirs) {
    TempDir d;
    d.write("a.json", bufBlueprint("a"));
    d.write("sub/b.json", bufBlueprint("b"));
    d.write("sub/deeper/c.json", bufBlueprint("c"));

    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, d.path.string(), 1, imported, error)) << error;
    EXPECT_EQ(imported, 2);
    EXPECT_TRUE(ctx.library().exists("a"));
    EXPECT_TRUE(ctx.library().exists("b"));
    EXPECT_FALSE(ctx.library().exists("c"));
}

TEST(LibraryScan, NegativeDepthIsUnlimited) {
    TempDir d;
    d.write("a.json", bufBlueprint("a"));
    d.write("x/y/z/c.json", bufBlueprint("c"));

    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, d.path.string(), -1, imported, error)) << error;
    EXPECT_EQ(imported, 2);
    EXPECT_TRUE(ctx.library().exists("c"));
}

// A 引用 sub/b.json 时 B 会随 A 一起导入；扫到 B.json 时按规范化路径识别为
// 同一个文件并跳过，不能误报重名，也不能用文件名再登记一个别名
TEST(LibraryScan, DependencyImportedThroughParentIsNotDuplicate) {
    TempDir d;
    nlohmann::json b = bufBlueprint("b");

    nlohmann::json a;
    a["definitions"]["b"] = "sub/b.json";
    nlohmann::json da;
    da["nets"] = {"i", "o"};
    da["components"] = nlohmann::json::array();
    da["expose"]["inputs"]["i"] = "i";
    da["expose"]["outputs"]["o"] = "o";
    a["definitions"]["a"] = da;

    d.write("a.json", a);
    d.write("sub/b.json", b);

    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, d.path.string(), -1, imported, error)) << error;
    EXPECT_TRUE(ctx.library().exists("a"));
    EXPECT_TRUE(ctx.library().exists("b"));
    EXPECT_EQ(ctx.library().definitions().size(), 2u) << "依赖文件不能被重复登记";
    EXPECT_EQ(imported, 2);
}

TEST(LibraryScan, SkipsCircuitsAndUnrelatedJson) {
    TempDir d;
    d.write("bp.json", bufBlueprint("bp"));
    d.write("top.json", nlohmann::json{{"$meta", {{"kind", "circuit"}, {"format", 1}}},
                                       {"nets", nlohmann::json::array()},
                                       {"components", nlohmann::json::array()}});
    d.writeText("readme.txt", "not json");

    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, d.path.string(), -1, imported, error)) << error;
    EXPECT_EQ(imported, 1);
    EXPECT_TRUE(ctx.library().exists("bp"));
    EXPECT_EQ(ctx.library().definitions().size(), 1u);
}

TEST(LibraryScan, DefinitionsOnlyFileIsImported) {
    TempDir d;
    // 老格式：没有 $meta.kind，根上只有 definitions
    nlohmann::json def = bufBlueprint("ignored");
    def.erase("$meta");
    nlohmann::json j;
    j["definitions"]["d1"] = def;
    d.write("defs.json", j);

    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, d.path.string(), -1, imported, error)) << error;
    EXPECT_EQ(imported, 1);
    EXPECT_TRUE(ctx.library().exists("d1"));
}

TEST(LibraryScan, DuplicateNameAcrossFilesIsRejected) {
    TempDir d;
    d.write("one.json", bufBlueprint("same"));
    d.write("two.json", bufBlueprint("same"));

    Context ctx;
    int imported = 0;
    std::string error;
    EXPECT_FALSE(importLibraryDirectory(ctx, d.path.string(), -1, imported, error));
    EXPECT_NE(error.find("重名"), std::string::npos) << error;
    EXPECT_TRUE(ctx.library().definitions().empty()) << "失败后必须整体回滚";
}

TEST(LibraryScan, MalformedJsonIsRejectedAndRolledBack) {
    TempDir d;
    d.write("good.json", bufBlueprint("good"));
    d.writeText("bad.json", "{ not valid json");

    Context ctx;
    int imported = 0;
    std::string error;
    EXPECT_FALSE(importLibraryDirectory(ctx, d.path.string(), -1, imported, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(ctx.library().definitions().empty()) << "失败后必须整体回滚";
}

TEST(LibraryScan, MissingDirectoryIsReported) {
    Context ctx;
    int imported = 0;
    std::string error;
    EXPECT_FALSE(importLibraryDirectory(ctx, "no/such/library/dir", -1, imported, error));
    EXPECT_NE(error.find("不存在"), std::string::npos) << error;
}

// --pins 走的路径：扫完库目录就能直接建 Composite 并 ensurePins 拿引脚，
// 不需要构造整个电路
TEST(LibraryScan, CompositeEnsurePinsAfterScan) {
    Context ctx;
    int imported = 0;
    std::string error;
    ASSERT_TRUE(importLibraryDirectory(ctx, DCS_LIBRARY_DIR, -1, imported, error)) << error;
    ASSERT_GT(imported, 0);

    CompositeComponent c("ha", "half_adder");
    ASSERT_TRUE(c.ensurePins(ctx, error)) << error;
    EXPECT_EQ(c.inputs().size(), 2u);
    EXPECT_EQ(c.outputs().size(), 2u);
    // 工具重复调用不能叠加引脚
    ASSERT_TRUE(c.ensurePins(ctx, error)) << error;
    EXPECT_EQ(c.inputs().size(), 2u);
    EXPECT_EQ(c.outputs().size(), 2u);
}
