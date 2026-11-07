// AXI4 桥的边界情况：复位、空闲、长 burst、未对齐首拍、反压
#include <gtest/gtest.h>

#include "fixture.h"

using namespace dsc::test;

TEST(Axi4BridgeEdge, IdleKeepsHandshakesLow) {
    Axi4Fixture f;
    f.compile();
    for (int i = 0; i < 5; i++) f.tick();
    EXPECT_EQ(f.get(f.bvalid), 0u);
    EXPECT_EQ(f.get(f.rvalid), 0u);
    EXPECT_EQ(f.get(f.wb_stb_o), 0u);
    EXPECT_EQ(f.get(f.wb_cyc_o), 0u);
}

TEST(Axi4BridgeEdge, ResetAbortsInFlightRead) {
    Axi4Fixture f;
    f.compile();
    // 发起读事务，但把 RREADY 拉低，让桥停在等待 R 握手的阶段
    f.set(f.rready, 0);
    f.set(f.araddr, 0x100);
    f.set(f.arlen, 0);
    f.set(f.arsize, 2);
    f.set(f.arburst, 1);
    f.set(f.arvalid, 1);
    for (int i = 0; i < 20 && !f.get(f.arready); i++) f.tick();
    ASSERT_TRUE(f.get(f.arready));
    f.set(f.arvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.rvalid));

    f.set(f.rst, 1);
    for (int i = 0; i < 3; i++) f.tick();
    EXPECT_EQ(f.get(f.rvalid), 0u) << "复位必须撤销在途读响应";
    EXPECT_EQ(f.get(f.wb_stb_o), 0u);
    f.set(f.rst, 0);
    f.set(f.rready, 1);
    f.tick();

    // 复位后仍能正常工作
    f.write(0x120, {0x5A5A5A5Au});
    EXPECT_EQ(f.peek(0x120), 0x5A5A5A5Au);
    auto v = f.read(0x120, 1);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0x5A5A5A5Au);
}

TEST(Axi4BridgeEdge, MaxIncrBurst256Beats) {
    Axi4Fixture f;
    f.compile();
    std::vector<uint32_t> w, r;
    for (uint32_t i = 0; i < 256; i++) w.push_back(0x1000u + i);
    f.write(0x1000, w);
    r = f.read(0x1000, 256);
    ASSERT_EQ(r.size(), 256u);
    for (uint32_t i = 0; i < 256; i++) EXPECT_EQ(r[i], 0x1000u + i) << "beat " << i;
}

TEST(Axi4BridgeEdge, UnalignedFirstBeat) {
    Axi4Fixture f;
    f.compile();
    // 起始地址 0x802、每拍 4 字节：首拍只用 lane2/3，之后按对齐地址 0x804 递增
    f.write(0x802, {0x33440000u, 0x55667788u}, 2, 1, 0, true, 0,
            {0xCu, 0xFu});
    EXPECT_EQ(f.peek(0x800) & 0xFFFF0000u, 0x33440000u) << "首拍只写 0x802、0x803";
    EXPECT_EQ(f.peek(0x804), 0x55667788u);
    EXPECT_EQ(f.peek(0x800) & 0x0000FFFFu, 0u) << "低两字节不应被写入";
}

TEST(Axi4BridgeEdge, WriteBResponseBackpressure) {
    Axi4Fixture f;
    f.compile();
    f.set(f.bready, 0);
    f.set(f.awaddr, 0x900);
    f.set(f.awlen, 0);
    f.set(f.awsize, 2);
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);
    f.set(f.wdata, 0xABCDEF01u);
    f.set(f.wstrb, 0xF);
    f.set(f.wlast, 1);
    f.set(f.wvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.wready); i++) f.tick();
    ASSERT_TRUE(f.get(f.wready));
    f.set(f.wvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.bvalid));
    for (int i = 0; i < 5; i++) f.tick();
    EXPECT_EQ(f.get(f.bvalid), 1u) << "BREADY 为低时 BVALID 必须保持";

    f.set(f.bready, 1);
    f.tick();
    EXPECT_EQ(f.get(f.bvalid), 0u);
    EXPECT_EQ(f.peek(0x900), 0xABCDEF01u);
}

TEST(Axi4BridgeEdge, ReadDataBackpressureKeepsBeatAndContinues) {
    Axi4Fixture f;
    f.compile();
    f.write(0xA00, {0x11111111u, 0x22222222u});
    f.set(f.rready, 0);
    f.set(f.araddr, 0xA00);
    f.set(f.arlen, 1);
    f.set(f.arsize, 2);
    f.set(f.arburst, 1);
    f.set(f.arvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.arready); i++) f.tick();
    ASSERT_TRUE(f.get(f.arready));
    f.set(f.arvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.rvalid));
    const uint32_t first = f.get(f.rdata);
    EXPECT_EQ(f.get(f.rlast), 0u);
    for (int i = 0; i < 5; i++) f.tick();
    EXPECT_EQ(f.get(f.rvalid), 1u) << "RREADY 为低时 RVALID 必须保持";
    EXPECT_EQ(f.get(f.rdata), first) << "RREADY 为低时数据必须稳定";

    f.set(f.rready, 1);
    f.tick();
    for (int i = 0; i < 50 && !f.get(f.rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.rvalid)) << "反压解除后应继续返回第二拍";
    EXPECT_EQ(f.get(f.rdata), 0x22222222u);
    EXPECT_EQ(f.get(f.rlast), 1u);
    f.tick();
    EXPECT_EQ(f.get(f.rvalid), 0u);
}

TEST(Axi4BridgeEdge, ZeroStrobeBeatCompletesWithoutWriting) {
    Axi4Fixture f;
    f.compile();
    f.write(0xB00, {0x12345678u});
    f.write(0xB00, {0xFFFFFFFFu}, 2, 1, 0, true, 0x0);
    EXPECT_EQ(f.peek(0xB00), 0x12345678u) << "WSTRB 全 0 不应改写存储器";
}
