// wishbone_ic 多主设备仲裁：设备主端口（virtio DMA）和 CPU 抢同一条总线时不能被饿死
#include "common.h"

using namespace dsc;
using namespace dsc_test;

namespace {

const char *kSegs = "0:4096,4096:4096";

// 协议正确的主设备：ack 后撤销一拍 stb 再发起下一笔。
// 从设备 s0 模拟成"看到 stb 下一拍就 ack"。
struct BusSim {
    IcFixture &f;
    int M;
    std::vector<bool> want;
    std::vector<bool> hold_off;
    std::vector<int> served;
    std::vector<uint8_t> prev_ack;
    std::vector<uint8_t> prev_s_stb;

    BusSim(IcFixture &fixture, int masters) : f(fixture), M(masters) {
        want.assign(M, true);
        hold_off.assign(M, false);
        served.assign(M, 0);
        prev_ack.assign(M, 0);
        prev_s_stb.assign(f.N, 0);
    }

    void step() {
        for (int mi = 0; mi < M; mi++) {
            const bool stb = want[mi] && !hold_off[mi];
            // 两个主设备都访问 s0 覆盖的地址，模拟抢同一块 RAM
            f.setM(mi, 0x100 + mi * 4, 0, false, 0xF, stb, stb);
        }
        for (int si = 0; si < f.N; si++)
            f.setS(si, 0, prev_s_stb[si]);
        f.tick();
        for (int si = 0; si < f.N; si++)
            prev_s_stb[si] = f.sStb(si);
        for (int mi = 0; mi < M; mi++) {
            const uint8_t a = f.mAck(mi);
            if (a && !prev_ack[mi]) {
                served[mi]++;
                hold_off[mi] = true; // 下一拍撤 stb，制造事务边界
            } else if (hold_off[mi]) {
                hold_off[mi] = false;
            }
            prev_ack[mi] = a;
        }
    }
};

} // namespace

TEST(WishboneIcArb, OnlyGrantedMasterCanDriveSlave) {
    IcFixture f(kSegs, 2);
    f.compile();

    // 两个主设备一个访 s0 一个访 s1，先到的 m0 拿到总线
    f.setM(0, 0x100, 0, false, 0xF, true, true);
    f.setM(1, 0x1000, 0, false, 0xF, true, true);
    f.tick(5);

    EXPECT_EQ(f.sStb(0), 1);
    EXPECT_EQ(f.sStb(1), 0);
}

TEST(WishboneIcArb, MasterKeepsBusUntilItReleasesStb) {
    IcFixture f(kSegs, 2);
    f.compile();

    f.setM(0, 0x100, 0, false, 0xF, true, true);
    f.tick(4);
    ASSERT_EQ(f.sStb(0), 1);

    // m0 没收到 ack 就不会撤销 stb，m1 此时不能把 m0 顶掉
    f.setM(1, 0x1000, 0, false, 0xF, true, true);
    f.tick(5);

    EXPECT_EQ(f.sStb(0), 1);
    EXPECT_EQ(f.sStb(1), 0);
}

TEST(WishboneIcArb, SecondMasterGetsBusAfterFirstReleases) {
    IcFixture f(kSegs, 2);
    f.compile();

    f.setM(0, 0x100, 0, false, 0xF, true, true);
    f.setM(1, 0x1000, 0, false, 0xF, true, true);
    f.tick(4);
    ASSERT_EQ(f.sStb(0), 1);

    f.releaseM(0);
    f.tick(6);

    EXPECT_EQ(f.sStb(0), 0);
    EXPECT_EQ(f.sStb(1), 1);
    EXPECT_EQ(f.sAdr(1), 0x1000u);
}

TEST(WishboneIcArb, BothMastersCompleteTransactions) {
    IcFixture f(kSegs, 2);
    f.compile();
    BusSim sim(f, 2);

    for (int t = 0; t < 120; t++)
        sim.step();

    // CPU 每拍都有请求，设备主端口也必须拿到总线，否则 DMA 请求永远完不成
    EXPECT_GT(sim.served[0], 3);
    EXPECT_GT(sim.served[1], 3);
}

TEST(WishboneIcArb, ThreeMastersAllGetServed) {
    IcFixture f("0:4096,4096:4096,8192:4096", 3);
    f.compile();
    BusSim sim(f, 3);

    for (int t = 0; t < 200; t++)
        sim.step();

    for (int mi = 0; mi < 3; mi++)
        EXPECT_GT(sim.served[mi], 2) << "master " << mi << " starved";
}

TEST(WishboneIcArb, AckIsNotForwardedToUngrantedMaster) {
    IcFixture f(kSegs, 2);
    f.compile();

    f.setM(0, 0x100, 0, false, 0xF, true, true);
    f.tick(4);
    f.setS(0, 0xA5A5, true);
    f.tick(3);

    EXPECT_EQ(f.mAck(0), 1);
    // m1 没拿到总线，从设备的应答不能串给它
    EXPECT_EQ(f.mAck(1), 0);
}

TEST(WishboneIcArb, RoundRobinStartsFromNextMaster) {
    IcFixture f(kSegs, 2);
    f.compile();

    f.setM(0, 0x100, 0, false, 0xF, true, true);
    f.setM(1, 0x1000, 0, false, 0xF, true, true);
    f.tick(4);
    ASSERT_EQ(f.sStb(0), 1);

    // m0 释放的同一拍 m1 还在请求，轮询应当直接轮到 m1，而不是又回到 m0
    f.releaseM(0);
    f.tick(6);
    EXPECT_EQ(f.sStb(1), 1);

    // m1 释放后重新拉高 m0，总线应回到 m0
    f.releaseM(1);
    f.tick(4);
    f.setM(0, 0x100, 0, false, 0xF, true, true);
    f.tick(6);
    EXPECT_EQ(f.sStb(0), 1);
    EXPECT_EQ(f.sStb(1), 0);
}
