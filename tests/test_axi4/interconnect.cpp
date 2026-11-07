// AXI4 互连：地址译码、burst 转发、ID 扩展/回送、多主、DECERR
#include <gtest/gtest.h>

#include "ic_fixture.h"

using namespace dsc::test;

TEST(Axi4IcTest, RoutesByAddress) {
    IcFixture f;
    f.compile();
    f.write(0, 0x100, {0x11111111u});
    f.write(0, 0x2100, {0x22222222u});
    EXPECT_EQ(f.peek(0, 0x100), 0x11111111u);
    EXPECT_EQ(f.peek(1, 0x100), 0x22222222u) << "0x2100 应落在第二个从口（段内偏移 0x100）";
    EXPECT_EQ(f.peek(0, 0x100), 0x11111111u) << "第二个从口的数据不能污染第一个";
}

TEST(Axi4IcTest, BurstForwardsThroughInterconnect) {
    IcFixture f;
    f.compile();
    f.write(0, 0x200, {1u, 2u, 3u, 4u});
    EXPECT_EQ(f.peek(0, 0x200), 1u);
    EXPECT_EQ(f.peek(0, 0x20C), 4u);
    auto v = f.read(0, 0x200, 4);
    ASSERT_EQ(v.size(), 4u);
    for (uint32_t i = 0; i < 4; i++) EXPECT_EQ(v[i], i + 1) << "beat " << i;
}

TEST(Axi4IcTest, IdIsPreservedAcrossInterconnect) {
    IcFixture f;
    f.compile();
    f.write(0, 0x300, {0xABCDEF01u}, 2, 1, 6);
    auto v = f.read(0, 0x300, 1, 2, 1, 7);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0xABCDEF01u);
}

TEST(Axi4IcTest, MultiMasterDifferentSlaves) {
    IcFixture f;
    f.compile();
    f.write(0, 0x400, {0xA0A0A0A0u});
    f.write(1, 0x2400, {0xB0B0B0B0u});
    EXPECT_EQ(f.peek(0, 0x400), 0xA0A0A0A0u);
    EXPECT_EQ(f.peek(1, 0x400), 0xB0B0B0B0u);
    auto v0 = f.read(0, 0x400, 1);
    auto v1 = f.read(1, 0x2400, 1);
    EXPECT_EQ(v0[0], 0xA0A0A0A0u);
    EXPECT_EQ(v1[0], 0xB0B0B0B0u);
}

TEST(Axi4IcTest, UnmappedWriteReturnsDecerr) {
    IcFixture f;
    f.compile();
    auto &n = f.mn[0];
    f.set(n.awid, 3);
    f.set(n.awaddr, 0x5000);
    f.set(n.awlen, 0);
    f.set(n.awsize, 2);
    f.set(n.awburst, 1);
    f.set(n.awvalid, 1);
    for (int i = 0; i < 200 && !f.get(n.awready); i++) f.tick();
    ASSERT_TRUE(f.get(n.awready));
    f.set(n.awvalid, 0);
    f.set(n.wdata, 0x12345678u);
    f.set(n.wstrb, 0xF);
    f.set(n.wlast, 1);
    f.set(n.wvalid, 1);
    for (int i = 0; i < 200 && !f.get(n.wready); i++) f.tick();
    ASSERT_TRUE(f.get(n.wready));
    f.set(n.wvalid, 0);
    for (int i = 0; i < 200 && !f.get(n.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(n.bvalid));
    EXPECT_EQ(f.get(n.bresp), 3u) << "未映射地址必须返回 DECERR";
    EXPECT_EQ(f.get(n.bid), 3u);
    f.tick();
    EXPECT_EQ(f.get(n.bvalid), 0u);
}

TEST(Axi4IcTest, UnmappedReadReturnsDecerr) {
    IcFixture f;
    f.compile();
    auto &n = f.mn[0];
    f.set(n.arid, 2);
    f.set(n.araddr, 0x5000);
    f.set(n.arlen, 0);
    f.set(n.arsize, 2);
    f.set(n.arburst, 1);
    f.set(n.arvalid, 1);
    for (int i = 0; i < 200 && !f.get(n.arready); i++) f.tick();
    ASSERT_TRUE(f.get(n.arready));
    f.set(n.arvalid, 0);
    for (int i = 0; i < 200 && !f.get(n.rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(n.rvalid));
    EXPECT_EQ(f.get(n.rresp), 3u) << "未映射读必须返回 DECERR";
    EXPECT_EQ(f.get(n.rlast), 1u);
    EXPECT_EQ(f.get(n.rid), 2u);
    f.tick();
    EXPECT_EQ(f.get(n.rvalid), 0u);
}

TEST(Axi4IcTest, WrapBurstForwardsAddressSequence) {
    IcFixture f;
    f.compile();
    f.write(0, 0x600, {1u, 2u, 3u, 4u});
    // WRAP 窗口是 16 字节，从 0x608 起 4 拍：0x608,0x60C,0x600,0x604
    auto v = f.read(0, 0x608, 4, 2, 2);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], 3u);
    EXPECT_EQ(v[1], 4u);
    EXPECT_EQ(v[2], 1u);
    EXPECT_EQ(v[3], 2u);
}

TEST(Axi4IcTest, NarrowTransferKeepsByteLane) {
    IcFixture f;
    f.compile();
    f.write(0, 0x700, {0xAAu, 0xBBu, 0xCCu, 0xDDu}, 0);
    EXPECT_EQ(f.peek(0, 0x700), 0xDDCCBBAAu);
    auto v = f.read(0, 0x700, 4, 0);
    ASSERT_EQ(v.size(), 4u);
    // 窄传输的数据落在地址对应的字节通道上
    EXPECT_EQ((v[0] >> 0) & 0xFF, 0xAAu);
    EXPECT_EQ((v[1] >> 8) & 0xFF, 0xBBu);
    EXPECT_EQ((v[2] >> 16) & 0xFF, 0xCCu);
    EXPECT_EQ((v[3] >> 24) & 0xFF, 0xDDu);
}

TEST(Axi4IcTest, DecerrReadCompletesAllBeats) {
    IcFixture f;
    f.compile();
    auto &n = f.mn[0];
    f.set(n.arid, 5);
    f.set(n.araddr, 0x9000);
    f.set(n.arlen, 3); // 4 拍
    f.set(n.arsize, 2);
    f.set(n.arburst, 1);
    f.set(n.arvalid, 1);
    for (int i = 0; i < 200 && !f.get(n.arready); i++) f.tick();
    ASSERT_TRUE(f.get(n.arready));
    f.set(n.arvalid, 0);
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 200 && !f.get(n.rvalid); k++) f.tick();
        ASSERT_TRUE(f.get(n.rvalid)) << "DECERR 读必须补满全部 beat，缺少第 " << i << " 拍";
        EXPECT_EQ(f.get(n.rresp), 3u);
        EXPECT_EQ(f.get(n.rid), 5u);
        EXPECT_EQ(f.get(n.rlast), (i == 3) ? 1u : 0u);
        f.tick();
    }
    for (int i = 0; i < 10 && f.get(n.rvalid); i++) f.tick();
    EXPECT_EQ(f.get(n.rvalid), 0u);
}

TEST(Axi4IcTest, DecerrWriteWaitsForLastBeat) {
    IcFixture f;
    f.compile();
    auto &n = f.mn[0];
    f.set(n.awid, 6);
    f.set(n.awaddr, 0x9000);
    f.set(n.awlen, 3);
    f.set(n.awsize, 2);
    f.set(n.awburst, 1);
    f.set(n.awvalid, 1);
    for (int i = 0; i < 200 && !f.get(n.awready); i++) f.tick();
    ASSERT_TRUE(f.get(n.awready));
    f.set(n.awvalid, 0);
    for (int i = 0; i < 4; i++) {
        f.set(n.wdata, 0x100u + (uint32_t) i);
        f.set(n.wstrb, 0xF);
        f.set(n.wlast, (i == 3) ? 1u : 0u);
        f.set(n.wvalid, 1);
        for (int k = 0; k < 200 && !f.get(n.wready); k++) f.tick();
        ASSERT_TRUE(f.get(n.wready)) << "DECERR 写必须收完所有 beat，卡在第 " << i << " 拍";
        EXPECT_EQ(f.get(n.bvalid), 0u) << "最后一个 beat 之前不能给写响应";
        f.set(n.wvalid, 0);
        f.tick();
    }
    for (int i = 0; i < 200 && !f.get(n.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(n.bvalid));
    EXPECT_EQ(f.get(n.bresp), 3u);
    EXPECT_EQ(f.get(n.bid), 6u);
    f.tick();
    EXPECT_EQ(f.get(n.bvalid), 0u);
}
