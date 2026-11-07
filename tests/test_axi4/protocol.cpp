// AXI4 桥的协议边界：复位打断在途写、AW/W 独立到达、响应时刻、WRAP 窗口起点、
// 非法 burst/size 不挂死
#include <gtest/gtest.h>

#include "fixture.h"

using namespace dsc::test;

TEST(Axi4BridgeProtocol, ResetAbortsInFlightWrite) {
    Axi4Fixture f;
    f.compile();
    // 发一笔 2 拍写，只送第一拍，让桥停在等第二拍
    f.set(f.awaddr, 0x100);
    f.set(f.awlen, 1);
    f.set(f.awsize, 2);
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);
    f.set(f.wdata, 0x11111111u);
    f.set(f.wstrb, 0xF);
    f.set(f.wlast, 0);
    f.set(f.wvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.wready); i++) f.tick();
    ASSERT_TRUE(f.get(f.wready));
    f.set(f.wvalid, 0);
    for (int i = 0; i < 5; i++) f.tick();
    EXPECT_EQ(f.get(f.bvalid), 0u) << "第二拍还没到，不能给写响应";

    f.set(f.rst, 1);
    for (int i = 0; i < 3; i++) f.tick();
    EXPECT_EQ(f.get(f.bvalid), 0u);
    EXPECT_EQ(f.get(f.wb_stb_o), 0u);
    EXPECT_EQ(f.get(f.wb_cyc_o), 0u);
    f.set(f.rst, 0);
    for (int i = 0; i < 2; i++) f.tick();

    // 复位后同一地址重新写完整两拍
    f.write(0x100, {0xAAAAAAAAu, 0xBBBBBBBBu});
    EXPECT_EQ(f.peek(0x100), 0xAAAAAAAAu);
    EXPECT_EQ(f.peek(0x104), 0xBBBBBBBBu);
}

TEST(Axi4BridgeProtocol, AwWithoutWStaysIdle) {
    Axi4Fixture f;
    f.compile();
    f.set(f.awaddr, 0x200);
    f.set(f.awlen, 1);
    f.set(f.awsize, 2);
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);
    // 只有地址、没有数据：不能发起 Wishbone 事务，也不能提前回响应
    for (int i = 0; i < 10; i++) {
        f.tick();
        EXPECT_EQ(f.get(f.wb_stb_o), 0u) << "W 未到就发起了总线事务";
        EXPECT_EQ(f.get(f.bvalid), 0u);
    }
    // 数据到达后正常完成
    for (uint32_t i = 0; i < 2; i++) {
        f.set(f.wdata, 0x2000u + i);
        f.set(f.wstrb, 0xF);
        f.set(f.wlast, (i == 1) ? 1u : 0u);
        f.set(f.wvalid, 1);
        for (int k = 0; k < 50 && !f.get(f.wready); k++) f.tick();
        ASSERT_TRUE(f.get(f.wready)) << "beat " << i;
        f.set(f.wvalid, 0);
        f.tick();
    }
    for (int i = 0; i < 50 && !f.get(f.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.bvalid));
    EXPECT_EQ(f.peek(0x200), 0x2000u);
    EXPECT_EQ(f.peek(0x204), 0x2001u);
}

TEST(Axi4BridgeProtocol, WWithoutAwIsNotAccepted) {
    Axi4Fixture f;
    f.compile();
    f.set(f.wdata, 0xDEADBEEFu);
    f.set(f.wstrb, 0xF);
    f.set(f.wlast, 1);
    f.set(f.wvalid, 1);
    for (int i = 0; i < 10; i++) {
        f.tick();
        EXPECT_EQ(f.get(f.wready), 0u) << "地址未到就接收了写数据";
    }
    f.set(f.awaddr, 0x300);
    f.set(f.awlen, 0);
    f.set(f.awsize, 2);
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.wready); i++) f.tick();
    ASSERT_TRUE(f.get(f.wready)) << "AW 到齐后应接收挂着的 W";
    f.set(f.wvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.bvalid));
    EXPECT_EQ(f.peek(0x300), 0xDEADBEEFu);
}

TEST(Axi4BridgeProtocol, BValidOnlyAfterLastBeat) {
    Axi4Fixture f;
    f.compile();
    f.set(f.awaddr, 0x400);
    f.set(f.awlen, 3); // 4 拍
    f.set(f.awsize, 2);
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);
    for (uint32_t i = 0; i < 4; i++) {
        f.set(f.wdata, 0x40000000u + i);
        f.set(f.wstrb, 0xF);
        f.set(f.wlast, (i == 3) ? 1u : 0u);
        f.set(f.wvalid, 1);
        for (int k = 0; k < 50 && !f.get(f.wready); k++) f.tick();
        ASSERT_TRUE(f.get(f.wready)) << "beat " << i;
        f.set(f.wvalid, 0);
        f.tick();
        if (i < 3)
            EXPECT_EQ(f.get(f.bvalid), 0u) << "第 " << i << " 拍之后提前给了写响应";
    }
    for (int i = 0; i < 50 && !f.get(f.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.bvalid));
    EXPECT_EQ(f.peek(0x40C), 0x40000003u);
}

TEST(Axi4BridgeProtocol, WrapStartingAtWindowBaseDoesNotWrap) {
    Axi4Fixture f;
    f.compile();
    f.write(0x400, {1u, 2u, 3u, 4u}, 2, 2); // 从窗口起点开始 WRAP
    EXPECT_EQ(f.peek(0x400), 1u);
    EXPECT_EQ(f.peek(0x404), 2u);
    EXPECT_EQ(f.peek(0x408), 3u);
    EXPECT_EQ(f.peek(0x40C), 4u);
    auto v = f.read(0x400, 4, 2, 2);
    ASSERT_EQ(v.size(), 4u);
    for (uint32_t i = 0; i < 4; i++) EXPECT_EQ(v[i], i + 1);
}

TEST(Axi4BridgeProtocol, ReadThenWriteImmediately) {
    Axi4Fixture f;
    f.compile();
    f.write(0x800, {0x11223344u});
    EXPECT_EQ(f.read(0x800, 1)[0], 0x11223344u);
    // 读事务刚结束就发下一笔写，状态机必须已经回到空闲
    f.write(0x800, {0x55667788u});
    EXPECT_EQ(f.peek(0x800), 0x55667788u);
    EXPECT_EQ(f.read(0x800, 1)[0], 0x55667788u);
}

TEST(Axi4BridgeProtocol, ReservedBurstTypeStillCompletes) {
    Axi4Fixture f;
    f.compile();
    // AxBURST=3 是保留值，主设备本来就不该发；桥不能因此挂死
    f.write(0x700, {0x0BADF00Du}, 2, 3);
    EXPECT_EQ(f.peek(0x700), 0x0BADF00Du);
}

TEST(Axi4BridgeProtocol, OversizedTransferStillCompletes) {
    Axi4Fixture f;
    f.compile();
    // 32 位总线上 AxSIZE=3（8 字节）超出数据总线宽度，属于主设备违规；
    // 桥按 8 字节步进地址，不挂死即可
    f.write(0x600, {0x11111111u, 0x22222222u}, 3, 1);
    EXPECT_EQ(f.peek(0x600), 0x11111111u);
    EXPECT_EQ(f.peek(0x608), 0x22222222u);
}
