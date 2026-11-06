// DLL 元件的边沿声明与伪时钟支持
#include <dcs/Circuit.h>
#include <dcs/components/platform/Dll.h>
#include <dcs/components/sequential/Sequential.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "common.h"

using namespace dsc;

namespace {

// 测试 DLL 没编出来时直接跳过，避免在有 TCC 但没构建 DLL 的环境里误报。
// GTEST_SKIP 只能在返回 void 的测试函数里用，不能塞进返回 bool 的辅助函数
#define REQUIRE_DLL(path) \
    if (!std::filesystem::exists(path)) { \
        GTEST_SKIP() << "找不到测试 DLL: " << path; \
    }

struct ClkDllFixture {
    Circuit c;
    DllComponent *dll = nullptr;
    ClockGen *gen = nullptr;
    Net *clk = nullptr;
    Net *out = nullptr;

    ClkDllFixture(const std::string &dll_name, PinEdge gen_edge) {
        auto d = std::make_unique<DllComponent>("cc", dllPathNamed(dll_name));
        dll = static_cast<DllComponent *>(c.addComponent(std::move(d)));
        auto g = std::make_unique<ClockGen>("g", 1, 1, gen_edge);
        gen = static_cast<ClockGen *>(c.addComponent(std::move(g)));
        clk = c.createNet("clk");
        out = c.createNet("count");
        c.connect(gen, "clk", clk);
        c.connect(dll, "clk", clk);
        c.connect(dll, "count", out);
    }

    uint8_t count() {
        return c.getWire(out->id())[0];
    }
};

} // namespace

TEST(DllClocking, InputEdgeDeclarations) {
    auto path = dllPathNamed("dll_test_clk");
    REQUIRE_DLL(path);

    Circuit c;
    auto d = std::make_unique<DllComponent>("cc", path);
    auto *dll = static_cast<DllComponent *>(c.addComponent(std::move(d)));
    ASSERT_EQ(dll->inputs().size(), 2u);
    EXPECT_EQ(dll->inputs()[0]->name(), "clk");
    EXPECT_EQ(dll->inputs()[0]->edge(), PinEdge::Rising);
    EXPECT_EQ(dll->inputs()[1]->name(), "rst");
    EXPECT_EQ(dll->inputs()[1]->edge(), PinEdge::Falling);
}

TEST(DllClocking, PseudoClockTicksEveryTick) {
    auto path = dllPathNamed("dll_test_clk");
    REQUIRE_DLL(path);

    ClkDllFixture f("dll_test_clk", PinEdge::Rising);
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    for (int i = 0; i < 5; i++)
        f.c.tick();
    EXPECT_EQ(f.count(), 5u) << "伪时钟每个 tick 都应产生一个上升沿";
    EXPECT_EQ(f.c.getWire(f.clk->id())[0], 0) << "伪时钟线没有电平语义";
}

TEST(DllClocking, RealClockCountsRisingEdges) {
    auto path = dllPathNamed("dll_test_clk");
    REQUIRE_DLL(path);

    // clkgen 1/1：第一拍 seq 写高，第二拍 _w 才看到高电平，所以 6 拍里有
    // 3 个上升沿
    ClkDllFixture f("dll_test_clk", PinEdge::None);
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    for (int i = 0; i < 6; i++)
        f.c.tick();
    EXPECT_EQ(f.count(), 3u);
}

TEST(DllClocking, PseudoClockIgnoresWireLevel) {
    auto path = dllPathNamed("dll_test_clk");
    REQUIRE_DLL(path);

    ClkDllFixture f("dll_test_clk", PinEdge::Rising);
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    // 伪时钟线由 clkgen 驱动，外部写电平不会改变"每拍一个沿"的语义
    f.c.setWire(f.clk->id(), 1);
    f.c.tick();
    f.c.tick();
    EXPECT_EQ(f.count(), 2u);
}

TEST(DllClocking, PseudoClockRejectsOppositeEdge) {
    auto path = dllPathNamed("dll_test_clk");
    REQUIRE_DLL(path);

    ClkDllFixture f("dll_test_clk", PinEdge::Falling);
    auto e = f.c.compile();
    EXPECT_NE(e.code, ErrorCode::OK) << "下降沿伪时钟不能接到上升沿输入";
    EXPECT_NE(e.message.find("伪时钟"), std::string::npos) << e.message;
}

TEST(DllClocking, PseudoClockRequiresSetPseudo) {
    auto path = dllPathNamed("dll_test_clk_nopseudo");
    REQUIRE_DLL(path);

    ClkDllFixture f("dll_test_clk_nopseudo", PinEdge::Rising);
    auto e = f.c.compile();
    EXPECT_NE(e.code, ErrorCode::OK)
        << "DLL 声明了边沿但没导出 dcs_dll_set_pseudo，接伪时钟必须报错";
    EXPECT_NE(e.message.find("dcs_dll_set_pseudo"), std::string::npos) << e.message;
}

TEST(DllClocking, ResetClearsCounter) {
    auto path = dllPathNamed("dll_test_clk");
    REQUIRE_DLL(path);

    ClkDllFixture f("dll_test_clk", PinEdge::Rising);
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    for (int i = 0; i < 3; i++)
        f.c.tick();
    ASSERT_EQ(f.count(), 3u);
    f.c.reset();
    for (int i = 0; i < 2; i++)
        f.c.tick();
    EXPECT_EQ(f.count(), 2u);
}
