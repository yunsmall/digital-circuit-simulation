// DLL 元件重编译测试
#include <dcs/Circuit.h>
#include <dcs/components/platform/Dll.h>
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
    // deinit 后 JIT 里的 DLL 函数指针已经失效，不能再 tick；必须重新 compile
    EXPECT_FALSE(c.isCompiled());
    c.init(); // 未重新编译时 init 必须是 no-op，不能碰旧 JIT
    EXPECT_FALSE(c.isCompiled());

    // deinit 卸载了 DLL，重新编译必须在生成代码前重新绑定函数指针：
    // 只恢复 _loaded、不重新加载符号的话，JIT 符号表会拿到空地址，init 直接跳 0
    auto err_recompile = c.compile();
    ASSERT_TRUE(c.isCompiled()) << "重新编译失败: " << err_recompile.message;
    c.init();
    c.setWire(a->id(), {0x0F, 0});
    c.setWire(b->id(), {0x0F, 0});
    c.tick();
    EXPECT_EQ(c.getWire(y->id())[0], 0x0F);

    // 移除元件时旧 JIT 的 circuit_deinit 还会引用它的 DLL 状态；
    // 必须先完整 deinit 再销毁，DLL 引用计数才能归还
    c.removeComponent(dlp);
    EXPECT_FALSE(c.isCompiled());

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
