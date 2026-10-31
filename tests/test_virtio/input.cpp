// virtio-input：宿主 push 的 evdev 事件由 JIT 搬进 guest buffer
#include "common.h"

#include <dcs/components/platform/VirtioInput.h>

using namespace dsc;
using namespace dsc::test;

TEST(VirtioInput, DeliversEvents) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0); // F_WRITE
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    // 没有事件时不能消费队列
    for (int i = 0; i < 120; i++)
        f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 0u);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1})); // EV_KEY, KEY_A, 按下
    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 0})); // 松开
    for (int i = 0; i < 900; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1u);
    EXPECT_EQ(f.usedLen(s.used, 0), 16u);

    uint16_t type = 0, code = 0;
    int32_t value = -1;
    std::memcpy(&type, f.mem() + s.data, 2);
    std::memcpy(&code, f.mem() + s.data + 2, 2);
    std::memcpy(&value, f.mem() + s.data + 4, 4);
    EXPECT_EQ(type, 1);
    EXPECT_EQ(code, 30);
    EXPECT_EQ(value, 1);

    std::memcpy(&value, f.mem() + s.data + 12, 4);
    EXPECT_EQ(value, 0);
}

// buffer 小于一个事件时要立刻以 len=0 完成，不能把队列卡死
TEST(VirtioInput, TinyBufferCompletes) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 4, 2, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1}));
    for (int i = 0; i < 600; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1u);
    EXPECT_EQ(f.usedLen(s.used, 0), 0u);
}

// 三个 8 字节 buffer 各装一个事件：队列连续分发，used.idx 走到 3
TEST(VirtioInput, MultipleBuffers) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    for (int i = 0; i < 3; i++) {
        f.putDesc(s.desc, (uint16_t) i, s.data + i * 16, 8, 2, 0);
        f.put16(s.avail + 4 + 2 * i, (uint16_t) i);
    }
    prepareQueue(f, s, 3);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1}));
    ASSERT_TRUE(f.dev->pushInputEvent({1, 31, 1}));
    ASSERT_TRUE(f.dev->pushInputEvent({1, 32, 1}));
    for (int i = 0; i < 1500; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 3);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(f.usedLen(s.used, i), 8u);
        uint16_t code = 0;
        std::memcpy(&code, f.mem() + s.data + i * 16 + 2, 2);
        EXPECT_EQ(code, 30 + i);
    }
}

// 事件环满时 push 返回 false；设备消费后又能继续 push
TEST(VirtioInput, RingFull) {
    VirtioFixture<VirtioInput> f;
    // 环容量 64，留一个空位，最多放 63 个
    int pushed = 0;
    while (f.dev->pushInputEvent({1, (uint16_t) pushed, 0}))
        pushed++;
    EXPECT_EQ(pushed, 63);

    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 2, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);
    for (int i = 0; i < 600; i++)
        f.tick();
    EXPECT_EQ(f.usedLen(s.used, 0), 8u);
    EXPECT_TRUE(f.dev->pushInputEvent({1, 99, 0}));
}

// 环回绕：推满再消费，事件顺序不能乱
TEST(VirtioInput, RingWraparound) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0);
    prepareQueue(f, s, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8);
    f.write(0x050, 0);

    // 先塞 40 个，设备每次取 8 个，取完后 tail 前进 40
    for (int i = 0; i < 40; i++)
        ASSERT_TRUE(f.dev->pushInputEvent({1, (uint16_t) i, 0}));
    for (int round = 0; round < 5; round++) {
        f.put16(s.avail + 2, (uint16_t) (round + 1));
        f.put16(s.avail + 4 + 2 * (round % 8), 0);
        for (int i = 0; i < 400; i++)
            f.tick();
    }
    EXPECT_EQ(f.get16(s.used + 2), 5);

    // 再塞 40 个（跨过环边界），code 应该继续递增
    for (int i = 40; i < 80; i++)
        ASSERT_TRUE(f.dev->pushInputEvent({1, (uint16_t) i, 0}));
    for (int round = 5; round < 10; round++) {
        f.put16(s.avail + 2, (uint16_t) (round + 1));
        f.put16(s.avail + 4 + 2 * (round % 8), 0);
        for (int i = 0; i < 400; i++)
            f.tick();
    }
    EXPECT_EQ(f.get16(s.used + 2), 10);
}
