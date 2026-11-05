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

// addr+len 溢出 size_t 时 dump 打印的地址会回绕，必须在解析阶段拒绝
TEST(CliParse, DumpMemAddrLenOverflow) {
    size_t addr = 0, len = 0;
    std::string err;
    EXPECT_FALSE(dcs_cli::parseAddrLen("FFFFFFFFFFFFFFF0:20", addr, len, err));
    EXPECT_FALSE(err.empty());
}

// --load FILE@ADDR：地址支持十进制/0x；文件名里可能有 '@'，从最后一个拆
TEST(CliParse, LoadSpec) {
    std::string path, err;
    size_t addr = 0;
    EXPECT_TRUE(dcs_cli::parseLoadSpec("fw.bin@0x80000000", path, addr, err)) << err;
    EXPECT_EQ(path, "fw.bin");
    EXPECT_EQ(addr, 0x80000000u);

    EXPECT_TRUE(dcs_cli::parseLoadSpec("dir@x/fw.bin@16", path, addr, err)) << err;
    EXPECT_EQ(path, "dir@x/fw.bin");
    EXPECT_EQ(addr, 16u);

    EXPECT_FALSE(dcs_cli::parseLoadSpec("fw.bin", path, addr, err));
    EXPECT_FALSE(dcs_cli::parseLoadSpec("@0x10", path, addr, err));
    EXPECT_FALSE(dcs_cli::parseLoadSpec("fw.bin@", path, addr, err));
    EXPECT_FALSE(dcs_cli::parseLoadSpec("fw.bin@0xZZ", path, addr, err));
}

// --set NET=VALUE：十六进制右对齐到 128 位槽，十进制走 uint64
TEST(CliParse, WireSet) {
    std::string net, err;
    std::vector<uint8_t> bytes;

    EXPECT_TRUE(dcs_cli::parseWireSet("start_pc=0x80000000", net, bytes, err)) << err;
    EXPECT_EQ(net, "start_pc");
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x80);
    EXPECT_EQ(bytes[4], 0x00);

    EXPECT_TRUE(dcs_cli::parseWireSet("n=1", net, bytes, err)) << err;
    EXPECT_EQ(bytes[0], 1);
    EXPECT_EQ(bytes[1], 0);

    EXPECT_TRUE(dcs_cli::parseWireSet("n=0x1234", net, bytes, err)) << err;
    EXPECT_EQ(bytes[0], 0x34);
    EXPECT_EQ(bytes[1], 0x12);

    // 32 个十六进制数字 = 128 位上限；再多一位必须拒绝
    EXPECT_TRUE(dcs_cli::parseWireSet("n=0x" + std::string(32, 'F'), net, bytes, err)) << err;
    EXPECT_EQ(bytes[15], 0xFF);
    EXPECT_FALSE(dcs_cli::parseWireSet("n=0x" + std::string(33, 'F'), net, bytes, err));

    EXPECT_FALSE(dcs_cli::parseWireSet("n", net, bytes, err));
    EXPECT_FALSE(dcs_cli::parseWireSet("=1", net, bytes, err));
    EXPECT_FALSE(dcs_cli::parseWireSet("n=0xGG", net, bytes, err));
}
