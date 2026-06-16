// 测试 DllComponent — 加载外部 DLL 元件
#include <dcs/Circuit.h>
#include <dcs/components/Dll.h>
#include <dcs/util/ExePath.h>
#include <filesystem>
#include <gtest/gtest.h>

static std::string dllPath() {
    namespace fs = std::filesystem;
    auto dir = fs::path(dsc::exeDir());
    auto dll = dir / "dll_test_and.dll";
    if (fs::exists(dll))
        return dll.string();
    if (fs::exists("dll_test_and.dll"))
        return "dll_test_and.dll";
    return dll.string();
}

TEST(DllTest, LoadAndGate) {
    auto path = dllPath();
    ASSERT_TRUE(std::filesystem::exists(path)) << "找不到测试 DLL: " << path;

    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *y = c.createNet("y");
    auto dl = std::make_unique<dsc::DllComponent>("and_gate", path);
    auto *dlp = c.addComponent(std::move(dl));
    c.connect(dlp, "a", a);
    c.connect(dlp, "b", b);
    c.connect(dlp, "y", y);

    c.compile();
    ASSERT_TRUE(c.isCompiled()) << "编译失败: " << c.error();
    c.init();

    c.setWire(a->id(), {0, 0});
    c.setWire(b->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y->id())[0], 0);

    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y->id())[0], 0x0F);

    c.setWire(a->id(), {0xAA, 0});
    c.setWire(b->id(), {0x55, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y->id())[0], 0x00);

    c.setWire(a->id(), {0x12, 0});
    c.setWire(b->id(), {0x34, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y->id())[0], 0x10);
}

TEST(DllTest, Recompile) {
    auto path = dllPath();
    ASSERT_TRUE(std::filesystem::exists(path));

    dsc::Circuit c;
    auto *a = c.createNet("a"), *b = c.createNet("b"), *y = c.createNet("y");
    auto dl = std::make_unique<dsc::DllComponent>("and_gate", path);
    auto *dlp = c.addComponent(std::move(dl));
    c.connect(dlp, "a", a);
    c.connect(dlp, "b", b);
    c.connect(dlp, "y", y);

    c.compile();
    ASSERT_TRUE(c.isCompiled()) << "编译失败: " << c.error();
    c.init();
    c.setWire(a->id(), {0xFF, 0});
    c.setWire(b->id(), {0xFF, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y->id())[0], 0xFF);

    c.deinit();
    c.clear();
    auto *a2 = c.createNet("a2"), *b2 = c.createNet("b2"), *y2 = c.createNet("y2");
    auto dl2 = std::make_unique<dsc::DllComponent>("and_gate2", path);
    auto *dlp2 = c.addComponent(std::move(dl2));
    c.connect(dlp2, "a", a2);
    c.connect(dlp2, "b", b2);
    c.connect(dlp2, "y", y2);

    c.compile();
    ASSERT_TRUE(c.isCompiled()) << "重新编译失败: " << c.error();
    c.init();
    c.setWire(a2->id(), {0x0F, 0});
    c.setWire(b2->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y2->id())[0], 0x0F);
}
