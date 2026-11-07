// AXI4 从设备桥的基本事务：单拍读写、INCR/WRAP/FIXED burst、WSTRB、ID 回送
#include <gtest/gtest.h>

#include "fixture.h"

using namespace dsc::test;

TEST(Axi4BridgeTest, SingleWriteThenRead) {
    Axi4Fixture f;
    f.compile();
    f.write(0x100, {0x12345678u});
    EXPECT_EQ(f.peek(0x100), 0x12345678u);
    auto v = f.read(0x100, 1);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0x12345678u);
}

TEST(Axi4BridgeTest, IncrBurstWritesAndReads) {
    Axi4Fixture f;
    f.compile();
    f.write(0x200, {0x11u, 0x22u, 0x33u, 0x44u});
    EXPECT_EQ(f.peek(0x200), 0x11u);
    EXPECT_EQ(f.peek(0x204), 0x22u);
    EXPECT_EQ(f.peek(0x208), 0x33u);
    EXPECT_EQ(f.peek(0x20C), 0x44u);
    auto v = f.read(0x200, 4);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], 0x11u);
    EXPECT_EQ(v[1], 0x22u);
    EXPECT_EQ(v[2], 0x33u);
    EXPECT_EQ(v[3], 0x44u);
}

TEST(Axi4BridgeTest, WriteStrobeMasksBytes) {
    Axi4Fixture f;
    f.compile();
    f.write(0x300, {0xAABBCCDDu});
    f.write(0x300, {0x11223344u}, 2, 1, 0, true, 0x5); // 只写 byte0 和 byte2
    // 0xAABBCCDD 的内存字节是 DD CC BB AA；写 byte0=0x44、byte2=0x22 后为 44 CC 22 AA
    EXPECT_EQ(f.peek(0x300), 0xAA22CC44u);
}

TEST(Axi4BridgeTest, FixedBurstRepeatsAddress) {
    Axi4Fixture f;
    f.compile();
    f.write(0x400, {1u, 2u, 3u}, 2, 0);
    EXPECT_EQ(f.peek(0x400), 3u);
    EXPECT_EQ(f.peek(0x404), 0u) << "FIXED burst 不应推进地址";
}

TEST(Axi4BridgeTest, WrapBurstWrapsAtBoundary) {
    Axi4Fixture f;
    f.compile();
    // 16 字节容器内从 0x408 开始写 4 个 word：0x408,0x40C,0x400,0x404
    f.write(0x408, {0x10u, 0x20u, 0x30u, 0x40u}, 2, 2);
    EXPECT_EQ(f.peek(0x408), 0x10u);
    EXPECT_EQ(f.peek(0x40C), 0x20u);
    EXPECT_EQ(f.peek(0x400), 0x30u);
    EXPECT_EQ(f.peek(0x404), 0x40u);
}

TEST(Axi4BridgeTest, IdIsEchoed) {
    Axi4Fixture f;
    f.compile();
    f.write(0x500, {0xDEADBEEFu}, 2, 1, 7);
    auto v = f.read(0x500, 1, 2, 1, 9);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0xDEADBEEFu);
}

TEST(Axi4BridgeTest, NarrowByteTransfers) {
    Axi4Fixture f;
    f.compile();
    // SIZE=0（1 字节），INCR：写 0x600..0x603 四个字节
    f.write(0x600, {0xAAu, 0xBBu, 0xCCu, 0xDDu}, 0, 1);
    EXPECT_EQ(f.peek(0x600), 0xDDCCBBAAu);
    auto v = f.read(0x600, 4, 0, 1);
    ASSERT_EQ(v.size(), 4u);
    // 数据落在地址对应的字节通道上，读回整字后按通道取字节
    EXPECT_EQ((v[0] >> 0) & 0xFF, 0xAAu);
    EXPECT_EQ((v[1] >> 8) & 0xFF, 0xBBu);
    EXPECT_EQ((v[2] >> 16) & 0xFF, 0xCCu);
    EXPECT_EQ((v[3] >> 24) & 0xFF, 0xDDu);
}

TEST(Axi4BridgeTest, WriteDataBeforeAddress) {
    Axi4Fixture f;
    f.compile();
    f.write(0x700, {0x55AA55AAu}, 2, 1, 3, false);
    EXPECT_EQ(f.peek(0x700), 0x55AA55AAu);
}
