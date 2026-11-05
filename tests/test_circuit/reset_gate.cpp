// reset() 和 tick/setWire 一样受 _compiled 门禁保护：电路改动或 deinit 之后，
// 旧 JIT 代码里硬编码的元件地址已经失效，不能再执行
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>

using namespace dsc;

TEST(CircuitReset, AfterRemoveComponentIsGated) {
    Circuit c;
    auto *mem = static_cast<WishboneMemory *>(c.addComponent(std::make_unique<WishboneMemory>("m", 4, 32)));
    ASSERT_NE(mem, nullptr);
    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    ASSERT_TRUE(c.isCompiled());
    c.init();

    c.removeComponent(mem);
    ASSERT_FALSE(c.isCompiled());
    c.reset(); // 旧实现会执行 circuit_reset，访问已释放的存储
    EXPECT_FALSE(c.isCompiled()) << "被门禁拦下后不应改变状态";
}

TEST(CircuitReset, AfterDeinitIsGated) {
    Circuit c;
    auto *mem = static_cast<WishboneMemory *>(c.addComponent(std::make_unique<WishboneMemory>("m", 4, 32)));
    ASSERT_NE(mem, nullptr);
    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    c.init();
    c.deinit();
    ASSERT_FALSE(c.isCompiled());

    c.reset(); // DLL/外部资源已经释放，旧 reset 代码不能跑
    EXPECT_FALSE(c.isCompiled());
}
