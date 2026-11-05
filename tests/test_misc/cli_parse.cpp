// CLI 参数解析：非法输入必须返回错误而不是抛异常
#include <gtest/gtest.h>

#include "../../src/bin/cli_common.h"

TEST(CliParse, DumpMemAddrLen) {
    size_t addr = 0, len = 0;
    std::string err;

    EXPECT_TRUE(dcs_cli::parseAddrLen("1000:20", addr, len, err)) << err;
    EXPECT_EQ(addr, 0x1000u);
    EXPECT_EQ(len, 0x20u);

    // 0x 前缀是用户最自然的写法，--dump-mem 输出里也是十六进制
    EXPECT_TRUE(dcs_cli::parseAddrLen("0x80000000:0x100", addr, len, err)) << err;
    EXPECT_EQ(addr, 0x80000000u);
    EXPECT_EQ(len, 0x100u);

    EXPECT_FALSE(dcs_cli::parseAddrLen("1000", addr, len, err));
    EXPECT_FALSE(err.empty());
    // --dump-mem 是十六进制，abc/def 本身合法；用带非十六进制字符的串
    EXPECT_FALSE(dcs_cli::parseAddrLen("xyz:10", addr, len, err));
    EXPECT_FALSE(dcs_cli::parseAddrLen("1000:xyz", addr, len, err));
    EXPECT_FALSE(dcs_cli::parseAddrLen(":10", addr, len, err));
    EXPECT_FALSE(dcs_cli::parseAddrLen("1000:", addr, len, err)) << "空长度不能当 0 处理";
}
