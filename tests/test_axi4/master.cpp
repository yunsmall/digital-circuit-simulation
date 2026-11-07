// AXI4 主设备适配器 + 从设备桥的端到端测试：mem_* 事务经完整 AXI4 通道落盘
#include <gtest/gtest.h>

#include "master_fixture.h"

using namespace dsc::test;

TEST(Axi4MasterTest, WriteThenReadRoundTrip) {
    MasterFixture f;
    f.compile();
    f.write(0x100, 0x12345678u);
    EXPECT_EQ(f.peek(0x100), 0x12345678u);
    EXPECT_EQ(f.read(0x100), 0x12345678u);
}

TEST(Axi4MasterTest, ByteStrobeWrite) {
    MasterFixture f;
    f.compile();
    f.write(0x200, 0xAABBCCDDu);
    f.write(0x200, 0x11223344u, 0x5);
    EXPECT_EQ(f.peek(0x200), 0xAA22CC44u);
}

TEST(Axi4MasterTest, UnalignedAddressMovesByteLane) {
    MasterFixture f;
    f.compile();
    // CPU 语义：sel bit i 对应 adr+i，数据在低字节；适配器负责搬到 AXI 的字节通道
    f.write(0x302, 0x00003344u, 0x3);
    EXPECT_EQ(f.peek(0x300) & 0xFFFF0000u, 0x33440000u);
    EXPECT_EQ(f.peek(0x300) & 0xFFFFu, 0u);
}

TEST(Axi4MasterEdge, SelZeroWritesNothing) {
    MasterFixture f;
    f.compile();
    f.write(0x400, 0xAABBCCDDu);
    f.write(0x400, 0xFFFFFFFFu, 0x0);
    EXPECT_EQ(f.peek(0x400), 0xAABBCCDDu) << "sel=0 不应改动存储器";
    EXPECT_EQ(f.read(0x400), 0xAABBCCDDu) << "sel=0 的写事务仍必须正常应答";
}

TEST(Axi4MasterEdge, LaneBitsShiftedOutOfWordAreDropped) {
    MasterFixture f;
    f.compile();
    f.write(0x500, 0x11111111u);
    // adr=0x503、sel bit3 指向 0x506，已经出了这个字，移位后自然被掩掉
    f.write(0x503, 0xFFFFFFFFu, 0x8);
    EXPECT_EQ(f.peek(0x500), 0x11111111u) << "越出字的选通位必须丢弃";
}

TEST(Axi4MasterEdge, PartialWordAtOffsetMapsBytesByAddress) {
    MasterFixture f;
    f.compile();
    f.write(0x600, 0xAAAAAAAAu);
    // adr=0x601、sel=0x6 表示写 0x602、0x603，字节来自 wdat 的第 1、2 字节
    f.write(0x601, 0x11223344u, 0x6);
    EXPECT_EQ(f.peek(0x600), 0x2233AAAAu);
}

TEST(Axi4MasterEdge, AckStaysHighUntilReqDrops) {
    MasterFixture f;
    f.compile();
    f.set(f.mem_adr, 0x700);
    f.set(f.mem_wdat, 0xCAFEF00Du);
    f.set(f.mem_sel, 0xF);
    f.set(f.mem_we, 1);
    f.set(f.mem_req, 1);
    for (int i = 0; i < 300 && !f.get(f.mem_ack); i++) f.tick();
    ASSERT_TRUE(f.get(f.mem_ack));
    // 请求保持期间应答保持，且不会再发第二笔事务
    for (int i = 0; i < 10; i++) {
        f.tick();
        ASSERT_EQ(f.get(f.mem_ack), 1u) << "mem_req 未撤销时 mem_ack 必须保持";
    }
    EXPECT_EQ(f.peek(0x700), 0xCAFEF00Du);
    f.set(f.mem_req, 0);
    for (int i = 0; i < 10 && f.get(f.mem_ack); i++) f.tick();
    EXPECT_EQ(f.get(f.mem_ack), 0u);
}

TEST(Axi4MasterEdge, NextTransactionStartsAfterReqDrops) {
    MasterFixture f;
    f.compile();
    for (uint32_t i = 0; i < 3; i++) {
        f.write(0x800 + i * 4, 0x100u + i);
        EXPECT_EQ(f.peek(0x800 + i * 4), 0x100u + i) << "第 " << i << " 笔事务";
    }
    for (uint32_t i = 0; i < 3; i++) EXPECT_EQ(f.read(0x800 + i * 4), 0x100u + i);
}

TEST(Axi4MasterEdge, ResetMidTransactionRecovers) {
    MasterFixture f;
    f.compile();
    f.set(f.mem_adr, 0x900);
    f.set(f.mem_wdat, 0x55667788u);
    f.set(f.mem_sel, 0xF);
    f.set(f.mem_we, 1);
    f.set(f.mem_req, 1);
    f.tick();
    f.tick();
    ASSERT_EQ(f.get(f.mem_ack), 0u) << "事务还没完成，不应已有应答";

    f.set(f.rst, 1);
    for (int i = 0; i < 3; i++) f.tick();
    EXPECT_EQ(f.get(f.mem_ack), 0u);
    f.set(f.rst, 0);
    f.set(f.mem_req, 0);
    f.set(f.mem_we, 0);
    for (int i = 0; i < 3; i++) f.tick();

    // 复位后重新发起同一笔事务
    f.write(0x900, 0x55667788u);
    EXPECT_EQ(f.peek(0x900), 0x55667788u);
    EXPECT_EQ(f.read(0x900), 0x55667788u);
}
