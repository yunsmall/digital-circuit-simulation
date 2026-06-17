// DLL 元件重编译测试
#include <dcs/Circuit.h>
#include <dcs/components/Dll.h>
#include <gtest/gtest.h>
#include "common.h"

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

    auto err = c.compile();
    ASSERT_TRUE(c.isCompiled()) << "编译失败: " << err.message;
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
    auto err2 = c.compile();
    ASSERT_TRUE(c.isCompiled()) << "重新编译失败: " << err2.message;
    c.init();
    c.setWire(a2->id(), {0x0F, 0});
    c.setWire(b2->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y2->id())[0], 0x0F);
}
