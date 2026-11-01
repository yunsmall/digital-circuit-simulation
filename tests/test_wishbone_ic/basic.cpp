// wishbone_ic 基本功能：地址路由、应答转发、事务边界
#include "common.h"

using namespace dsc;
using namespace dsc_test;

namespace {

// 两段地址：0x0000~0x0FFF -> s0，0x1000~0x1FFF -> s1
const char *kSegs = "0:4096,4096:4096";

} // namespace

TEST(WishboneIc, RoutesAddressToMatchingSegment) {
    IcFixture f(kSegs, 1);
    f.compile();

    f.setM(0, 0x1000, 0x1234, false, 0xF, true, true);
    f.tick(4);

    EXPECT_EQ(f.sStb(0), 0);
    EXPECT_EQ(f.sStb(1), 1);
    EXPECT_EQ(f.sAdr(1), 0x1000u);
    EXPECT_EQ(f.sCyc(1), 1);
}

TEST(WishboneIc, RoutesSecondSegmentAndPassesWriteStrobe) {
    IcFixture f(kSegs, 1);
    f.compile();

    f.setM(0, 0x1004, 0xDEADBEEF, true, 0x3, true, true);
    f.tick(4);

    EXPECT_EQ(f.sStb(1), 1);
    EXPECT_EQ(f.sAdr(1), 0x1004u);
    EXPECT_EQ(f.sDat(1), 0xDEADBEEFu);
    EXPECT_EQ(f.sWe(1), 1);
    EXPECT_EQ(f.sSel(1), 0x3);
}

TEST(WishboneIc, UnmatchedAddressFallsBackToFirstSegment) {
    IcFixture f(kSegs, 1);
    f.compile();

    f.setM(0, 0x9000, 0x55, false, 0xF, true, true);
    f.tick(4);

    EXPECT_EQ(f.sStb(0), 1);
    EXPECT_EQ(f.sStb(1), 0);
    EXPECT_EQ(f.sAdr(0), 0x9000u);
}

TEST(WishboneIc, ForwardsSlaveDataAndAck) {
    IcFixture f(kSegs, 1);
    f.compile();

    f.setM(0, 0x1000, 0, false, 0xF, true, true);
    f.tick(4);
    f.setS(1, 0xCAFE1234, true);
    f.tick(3);

    EXPECT_EQ(f.mAck(0), 1);
    EXPECT_EQ(f.mDat(0), 0xCAFE1234u);
}

TEST(WishboneIc, DoesNotForwardWithoutCyc) {
    IcFixture f(kSegs, 1);
    f.compile();

    // stb 单独拉高而 cyc 为低不是合法事务，互连不能把它发给从设备
    f.setM(0, 0x1000, 0, false, 0xF, true, false);
    f.tick(4);

    EXPECT_EQ(f.sStb(0), 0);
    EXPECT_EQ(f.sStb(1), 0);
}

TEST(WishboneIc, ReleasesSlaveStbAfterMasterDropsStb) {
    IcFixture f(kSegs, 1);
    f.compile();

    f.setM(0, 0x1000, 0, false, 0xF, true, true);
    f.tick(4);
    ASSERT_EQ(f.sStb(1), 1);

    f.releaseM(0);
    f.tick(3);

    EXPECT_EQ(f.sStb(1), 0);
    EXPECT_EQ(f.sCyc(1), 0);
    EXPECT_EQ(f.mAck(0), 0);
}

TEST(WishboneIc, IdleBusDoesNotAssertAnySlaveStb) {
    IcFixture f(kSegs, 1);
    f.compile();

    f.tick(4);

    EXPECT_EQ(f.sStb(0), 0);
    EXPECT_EQ(f.sStb(1), 0);
    EXPECT_EQ(f.mAck(0), 0);
}
