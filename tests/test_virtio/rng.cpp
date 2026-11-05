// virtio-rng：设备把随机数写进驱动提供的缓冲区
#include "common.h"

#include <dcs/components/platform/VirtioRng.h>

using namespace dsc;
using namespace dsc::test;

TEST(VirtioRng, FillsBuffer) {
    VirtioFixture<VirtioRng> f;
    const uint32_t desc = 0x100, avail = 0x200, used = 0x300, data = 0x400;
    f.putDesc(desc, 0, data, 8, 2, 0); // F_WRITE
    f.put16(avail + 0, 0);
    f.put16(avail + 2, 1);
    f.put16(avail + 4, 0);
    f.put16(used + 0, 0);
    std::memset(f.mem() + data, 0, 8);

    f.configureQueue(desc, avail, used, 8);
    f.write(0x070, 1); // status
    f.write(0x050, 0); // notify

    for (int i = 0; i < 400; i++)
        f.tick();

    uint64_t v = 0;
    std::memcpy(&v, f.mem() + data, 8);
    EXPECT_NE(v, 0u);
    EXPECT_EQ(f.get16(used + 2), 1); // used.idx（vring_used 的 +0 是 flags）
    EXPECT_EQ(f.get16(used + 4), 0); // used.ring[0].id
    EXPECT_EQ(f.usedLen(used, 0), 8u);
}

// 大缓冲区：256 字节要跨 4 轮 64 字节的主端口事务
TEST(VirtioRng, LargeBuffer) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 256, 2, 0);
    std::memset(f.mem() + s.data, 0, 256);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    for (int i = 0; i < 1500; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 256u);
    // 高 64 字节也要被写到，不能只写第一块
    uint64_t tail = 0;
    std::memcpy(&tail, f.mem() + s.data + 248, 8);
    EXPECT_NE(tail, 0u);
}

// 链式描述符：两个可写段，written 是两段之和
TEST(VirtioRng, ChainedDescriptors) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 3, 1);     // NEXT|WRITE
    f.putDesc(s.desc, 1, s.data + 8, 8, 2, 0); // WRITE
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    for (int i = 0; i < 800; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 16u);
}

// 队列没 ready 时不处理请求
TEST(VirtioRng, IgnoresWhenNotReady) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 2, 0);
    prepareQueue(f, s, 1);
    f.write(0x030, 0);
    f.write(0x038, 8);
    f.write(0x080, s.desc);
    f.write(0x090, s.avail);
    f.write(0x0A0, s.used);
    f.write(0x070, 1);
    f.write(0x050, 0);

    for (int i = 0; i < 400; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 0u);
}

// 连续两个请求：used.idx 递增到 2，两个 used 条目各自记录长度
TEST(VirtioRng, TwoRequests) {
    VirtioFixture<VirtioRng> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 4, 2, 0);
    f.putDesc(s.desc, 1, s.data + 16, 4, 2, 0);
    prepareQueue(f, s, 2);
    f.put16(s.avail + 6, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    for (int i = 0; i < 1200; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 2);
    EXPECT_EQ(f.usedLen(s.used, 0), 4u);
    EXPECT_EQ(f.usedLen(s.used, 1), 4u);
}

// init 当重置重复调用时，后端 init 只能执行一次（后端 init 不可重入）
TEST(VirtioRng, RepeatedInitCallsBackendOnce) {
    struct Counters {
        int inits = 0;
        int deinits = 0;
    } ctr;
    dsc::Circuit c;
    auto dev = std::make_unique<VirtioRng>("vr0");
    auto *vp = dev.get();
    c.addComponent(std::move(dev));
    VirtioRng::BackendOps ops;
    ops.ctx = &ctr;
    ops.init = [](void *p) { ++static_cast<Counters *>(p)->inits; };
    ops.deinit = [](void *p) { ++static_cast<Counters *>(p)->deinits; };
    vp->setBackend(ops);
    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    c.init();
    EXPECT_EQ(ctr.inits, 1);
    c.init();
    EXPECT_EQ(ctr.inits, 1) << "第二次 init 不能再调后端 init";
    c.deinit();
    EXPECT_EQ(ctr.deinits, 1);
}
