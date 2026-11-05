// 搜索路径登记：重复目录只保留一份
#include <gtest/gtest.h>

#include <dcs/Context.h>

TEST(CircuitLibraryTest, SearchPathDedup) {
    dsc::Context ctx;
    auto &lib = ctx.library();
    lib.addSearchPath("a/b");
    lib.addSearchPath("a/b");
    lib.addSearchPath("");
    lib.addSearchPath("c");
    ASSERT_EQ(lib.searchPaths().size(), 2u);
    EXPECT_EQ(lib.searchPaths()[0], "a/b");
    EXPECT_EQ(lib.searchPaths()[1], "c");
}
