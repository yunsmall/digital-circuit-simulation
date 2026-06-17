// DLL 元件加载与基本功能测试
#include <dcs/Circuit.h>
#include <dcs/components/Dll.h>
#include <gtest/gtest.h>
#include "common.h"

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

    auto err = c.compile();
    ASSERT_TRUE(c.isCompiled()) << "编译失败: " << err.message;
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
