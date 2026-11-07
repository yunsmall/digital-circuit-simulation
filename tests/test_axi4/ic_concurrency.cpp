// AXI4 互连的并发与异常边界：复位、单主口、仲裁、读写通道并行、相同原始 ID、
// DECERR 之后继续工作
#include <gtest/gtest.h>

#include "ic_fixture.h"

using namespace dsc::test;

namespace {

void armWrite(IcFixtureT<2> &f, int m, uint32_t addr, uint32_t id, uint8_t len = 0) {
    auto &n = f.mn[m];
    f.set(n.awid, id);
    f.set(n.awaddr, addr);
    f.set(n.awlen, len);
    f.set(n.awsize, 2);
    f.set(n.awburst, 1);
    f.set(n.awvalid, 1);
}
void armRead(IcFixtureT<2> &f, int m, uint32_t addr, uint32_t id, uint8_t len = 0) {
    auto &n = f.mn[m];
    f.set(n.arid, id);
    f.set(n.araddr, addr);
    f.set(n.arlen, len);
    f.set(n.arsize, 2);
    f.set(n.arburst, 1);
    f.set(n.arvalid, 1);
}
void sendW(IcFixtureT<2> &f, int m, uint32_t data, bool last) {
    auto &n = f.mn[m];
    f.set(n.wdata, data);
    f.set(n.wstrb, 0xF);
    f.set(n.wlast, last ? 1u : 0u);
    f.set(n.wvalid, 1);
    for (int i = 0; i < 300 && !f.get(n.wready); i++) f.tick();
    ASSERT_TRUE(f.get(n.wready)) << "主口 " << m << " WREADY 未拉高";
    f.set(n.wvalid, 0);
    f.tick();
}

} // namespace

TEST(Axi4IcEdge, ResetDoesNotLeaveStaleResponse) {
    IcFixture f;
    f.compile();
    // 接收了 AW，但 W 还没送：事务挂在互连里
    armWrite(f, 0, 0x100, 1);
    for (int i = 0; i < 200 && !f.get(f.mn[0].awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[0].awready));
    f.set(f.mn[0].awvalid, 0);

    f.set(f.rst, 1);
    for (int i = 0; i < 3; i++) f.tick();
    EXPECT_EQ(f.get(f.mn[0].bvalid), 0u);
    EXPECT_EQ(f.get(f.mn[0].rvalid), 0u);
    EXPECT_EQ(f.get(f.mn[0].wready), 0u);
    f.set(f.rst, 0);
    for (int i = 0; i < 2; i++) f.tick();

    // 复位后旧事务不能残留，新事务正常工作
    f.write(0, 0x100, {0xA5A5A5A5u});
    EXPECT_EQ(f.peek(0, 0x100), 0xA5A5A5A5u);
    EXPECT_EQ(f.read(0, 0x100, 1)[0], 0xA5A5A5A5u);
}

TEST(Axi4IcEdge, SingleMasterWorksAndDecerrEchoesId) {
    IcFixtureT<1> f;
    f.compile();
    f.write(0, 0x300, {0xCAFEBABEu});
    EXPECT_EQ(f.peek(0, 0x300), 0xCAFEBABEu);
    EXPECT_EQ(f.read(0, 0x300, 1)[0], 0xCAFEBABEu);

    // 单主口时附加 ID 位为 0，DECERR 响应仍要带回原始 ID
    f.write(0, 0x5000, {0u}, 2, 1, 9);
    f.read(0, 0x5000, 1, 2, 1, 9);

    // DECERR 之后状态机必须回到空闲
    f.write(0, 0x300, {0x11223344u});
    EXPECT_EQ(f.peek(0, 0x300), 0x11223344u);
}

TEST(Axi4IcEdge, ArbitrationServesOneMasterAtATime) {
    IcFixture f;
    f.compile();
    f.set(f.mn[0].bready, 1);
    f.set(f.mn[1].bready, 1);
    armWrite(f, 0, 0x100, 1);
    armWrite(f, 1, 0x2100, 2);
    // 两个主口同时请求：只能有一个被受理
    for (int i = 0; i < 200 && !f.get(f.mn[0].awready) && !f.get(f.mn[1].awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[0].awready) || f.get(f.mn[1].awready));
    const bool first_is_0 = f.get(f.mn[0].awready) != 0;
    EXPECT_NE(f.get(f.mn[0].awready), f.get(f.mn[1].awready)) << "不能同时受理两个主口的写地址";
    const int first = first_is_0 ? 0 : 1;
    const int second = first_is_0 ? 1 : 0;

    // 完成第一个主口的事务
    f.set(f.mn[first].awvalid, 0);
    sendW(f, first, first_is_0 ? 0xA0A0A0A0u : 0xB0B0B0B0u, true);
    for (int i = 0; i < 300 && !f.get(f.mn[first].bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[first].bvalid)) << "第一个主口没收到写响应";
    f.tick();

    // 第二个主口挂着的 AW 必须被接走，事务不丢
    for (int i = 0; i < 300 && !f.get(f.mn[second].awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[second].awready)) << "第二个主口的地址请求被丢了";
    f.set(f.mn[second].awvalid, 0);
    sendW(f, second, first_is_0 ? 0xB0B0B0B0u : 0xA0A0A0A0u, true);
    for (int i = 0; i < 300 && !f.get(f.mn[second].bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[second].bvalid));

    EXPECT_EQ(f.peek(0, 0x100), 0xA0A0A0A0u);
    EXPECT_EQ(f.peek(1, 0x100), 0xB0B0B0B0u);
}

TEST(Axi4IcEdge, ReadAndWriteChannelsRunInParallel) {
    IcFixture f;
    f.compile();
    f.write(1, 0x2200, {0xDEADBEEFu});

    // 主口 0 往从口 0 写 4 拍
    armWrite(f, 0, 0x100, 1, 3);
    for (int i = 0; i < 200 && !f.get(f.mn[0].awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[0].awready));
    f.set(f.mn[0].awvalid, 0);

    // 读响应先按住不接，避免 RVALID 在两次轮询之间消失
    f.set(f.mn[1].rready, 0);
    armRead(f, 1, 0x2200, 2);
    bool read_seen = false, read_before_bresp = false;
    uint32_t read_data = 0, read_id = 0;
    for (uint32_t b = 0; b < 4; b++) {
        auto &n = f.mn[0];
        f.set(n.wdata, 0x1000u + b);
        f.set(n.wstrb, 0xF);
        f.set(n.wlast, (b == 3) ? 1u : 0u);
        f.set(n.wvalid, 1);
        for (int i = 0; i < 300 && !f.get(n.wready); i++) {
            f.tick();
            if (!read_seen && f.get(f.mn[1].rvalid)) {
                read_seen = true;
                read_before_bresp = (f.get(f.mn[0].bvalid) == 0);
                read_data = f.get(f.mn[1].rdata);
                read_id = f.get(f.mn[1].rid);
            }
        }
        ASSERT_TRUE(f.get(n.wready)) << "写 beat " << b;
        f.set(n.wvalid, 0);
        f.tick();
        if (!read_seen && f.get(f.mn[1].rvalid)) {
            read_seen = true;
            read_before_bresp = (f.get(f.mn[0].bvalid) == 0);
            read_data = f.get(f.mn[1].rdata);
            read_id = f.get(f.mn[1].rid);
        }
    }
    ASSERT_TRUE(read_seen) << "写事务进行中没有看到读响应";
    EXPECT_EQ(read_id, 2u);
    EXPECT_EQ(read_data, 0xDEADBEEFu);
    EXPECT_TRUE(read_before_bresp) << "读通道被写通道串行化了";
    f.set(f.mn[1].rready, 1);

    for (int i = 0; i < 300 && !f.get(f.mn[0].bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[0].bvalid));
    EXPECT_EQ(f.peek(0, 0x100), 0x1000u);
    EXPECT_EQ(f.peek(0, 0x10C), 0x1003u);
}

TEST(Axi4IcEdge, SameRawIdOnTwoMastersStaysSeparate) {
    IcFixture f;
    f.compile();
    f.write(1, 0x2200, {0x12345678u});

    armWrite(f, 0, 0x100, 5);
    for (int i = 0; i < 200 && !f.get(f.mn[0].awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[0].awready));
    f.set(f.mn[0].awvalid, 0);

    // 读响应按住不接，保证 RVALID 一定能被观察到
    f.set(f.mn[1].rready, 0);
    armRead(f, 1, 0x2200, 5);
    for (int i = 0; i < 200 && !f.get(f.mn[1].arready); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[1].arready));
    f.set(f.mn[1].arvalid, 0);

    sendW(f, 0, 0xCAFEF00Du, true);

    for (int i = 0; i < 300 && !f.get(f.mn[1].rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[1].rvalid));
    EXPECT_EQ(f.get(f.mn[1].rid), 5u) << "附加的主口编号必须被剥掉";
    EXPECT_EQ(f.get(f.mn[1].rdata), 0x12345678u);
    f.set(f.mn[1].rready, 1);
    f.tick();

    for (int i = 0; i < 300 && !f.get(f.mn[0].bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.mn[0].bvalid));
    EXPECT_EQ(f.get(f.mn[0].bid), 5u);
    f.tick();
    EXPECT_EQ(f.peek(0, 0x100), 0xCAFEF00Du);
}

TEST(Axi4IcEdge, DecerrFromSecondMasterKeepsRawId) {
    IcFixture f;
    f.compile();
    // 主口 1 的原始 ID 是 7；互连自己回 DECERR 时不能把扩展位带进 BID/RID
    f.write(1, 0x9000, {0x12345678u}, 2, 1, 7);
    auto v = f.read(1, 0x9000, 1, 2, 1, 7);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(f.get(f.mn[1].bvalid), 0u);
    EXPECT_EQ(f.get(f.mn[1].rvalid), 0u);

    // DECERR 之后两个主口都还能正常访问
    f.write(0, 0x100, {0xAAAA0001u});
    f.write(1, 0x2200, {0xBBBB0002u});
    EXPECT_EQ(f.peek(0, 0x100), 0xAAAA0001u);
    EXPECT_EQ(f.peek(1, 0x200), 0xBBBB0002u);
}

TEST(Axi4IcEdge, ThreeMastersExtendIdByTwoBits) {
    IcFixtureT<3> f;
    f.compile();
    // 3 个主口需要 2 位附加编号，从口 ID 宽度是 6 位
    f.write(2, 0x2200, {0x5A5A1234u}, 2, 1, 3);
    EXPECT_EQ(f.peek(1, 0x200), 0x5A5A1234u);
    auto v = f.read(2, 0x2200, 1, 2, 1, 3);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0x5A5A1234u);

    // 三主口轮流访问同一个从口，事务不能串
    for (uint32_t m = 0; m < 3; m++) {
        f.write((int) m, 0x100 + m * 4, {0x1000u + m}, 2, 1, (uint8_t) m);
    }
    for (uint32_t m = 0; m < 3; m++) {
        EXPECT_EQ(f.peek(0, 0x100 + m * 4), 0x1000u + m) << "主口 " << m;
        EXPECT_EQ(f.read((int) m, 0x100 + m * 4, 1, 2, 1, (uint8_t) m)[0], 0x1000u + m);
    }
}

TEST(Axi4IcEdge, MaxIdValueRoundTrip) {
    IcFixture f;
    f.compile();
    // id_width=4 时 0xF 是最大 ID，扩展后仍是合法编码
    f.write(0, 0x700, {0xFEEDFACEu}, 2, 1, 15);
    auto v = f.read(0, 0x700, 1, 2, 1, 15);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0xFEEDFACEu);
    EXPECT_EQ(f.peek(0, 0x700), 0xFEEDFACEu);
}
