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

// config 空间：设备名放在 0x100 的字里，载荷从 0x108 起按字节给
TEST(VirtioInput, ConfigName) {
    VirtioFixture<VirtioInput> f;
    // select=ID_NAME(1)、subsel=0；写 select/subsel 是单字节事务，测试里按整字写即可
    f.write(0x100, 0x01);
    const uint32_t hdr = f.read(0x100);
    EXPECT_EQ(hdr & 0xFFu, 1u);          // select
    EXPECT_EQ((hdr >> 16) & 0xFFu, 17u); // size = strlen("dcs-virtual-input")

    const uint32_t w0 = f.read(0x108);
    EXPECT_EQ(w0 & 0xFFu, 'd');
    EXPECT_EQ((w0 >> 8) & 0xFFu, 'c');
    EXPECT_EQ((w0 >> 16) & 0xFFu, 's');
    EXPECT_EQ((w0 >> 24) & 0xFFu, '-');
    // 超出 size 的部分读回 0
    EXPECT_EQ(f.read(0x120), 0u);
}

// config 空间：ID_DEVIDS 给出 bustype/vendor/product/version
TEST(VirtioInput, ConfigDevIds) {
    VirtioFixture<VirtioInput> f;
    f.write(0x100, 0x03);
    EXPECT_EQ((f.read(0x100) >> 16) & 0xFFu, 8u);
    // bustype=BUS_VIRTUAL(0x06)、vendor='D''C'（小端）
    EXPECT_EQ(f.read(0x108), 0x44430006u);
}

// ID_DEVIDS 只有 8 字节：越过 size 的读必须返回 0，不能漏出紧随其后的能力位图
TEST(VirtioInput, ConfigDevIdsDoesNotOverread) {
    VirtioFixture<VirtioInput> f;
    f.write(0x100, 0x03);
    EXPECT_EQ(f.read(0x108), 0x44430006u);
    EXPECT_EQ(f.read(0x110), 0u) << "0x110 已越过 ID_DEVIDS 的 8 字节";
    EXPECT_EQ(f.read(0x114), 0u);
}

// config 空间：EV_KEY 能力位图，Linux 靠它决定设备能发哪些按键
TEST(VirtioInput, ConfigEvKeyBits) {
    VirtioFixture<VirtioInput> f;
    f.write(0x100, 0x0111); // select=EV_BITS(0x11)、subsel=EV_KEY(1)
    EXPECT_EQ((f.read(0x100) >> 16) & 0xFFu, 8u);
    const uint32_t bits = f.read(0x108);
    // KEY_ESC(1) 在字节 1 的 bit1，KEY_ENTER(28)/KEY_A(30)/KEY_B(31) 在字节 3
    EXPECT_EQ(bits & 0xFFFFu, 0x0200u);
    EXPECT_EQ((bits >> 24) & 0xFFu, 0xD0u);
}

// config 空间：未知 select 不返回任何能力，驱动应把它当"没有这项"
TEST(VirtioInput, ConfigUnknownSelect) {
    VirtioFixture<VirtioInput> f;
    f.write(0x100, 0x7F);
    EXPECT_EQ((f.read(0x100) >> 16) & 0xFFu, 0u);
    EXPECT_EQ(f.read(0x108), 0u);
}

// status 队列：guest 回传的 LED 事件要能被宿主取走
TEST(VirtioInput, StatusQueue) {
    VirtioFixture<VirtioInput> f;
    VirtioInput::InputEvent ev{17, 0, 1}; // EV_LED, LED_NUML, 亮
    std::memcpy(f.mem() + 0x400, &ev, sizeof(ev));
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 0, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1);
    f.write(0x050, 1);

    for (int i = 0; i < 600; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1u);
    EXPECT_EQ(f.usedLen(s.used, 0), 0u);

    VirtioInput::InputEvent got{};
    ASSERT_TRUE(f.dev->popStatusEvent(got));
    EXPECT_EQ(got.type, 17);
    EXPECT_EQ(got.code, 0);
    EXPECT_EQ(got.value, 1);
    EXPECT_FALSE(f.dev->popStatusEvent(got));
}

// status 队列：装不下一个事件的 buffer 以 len=0 完成，也不能产生假事件
TEST(VirtioInput, StatusTinyBuffer) {
    VirtioFixture<VirtioInput> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 4, 0, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1);
    f.write(0x050, 1);

    for (int i = 0; i < 600; i++)
        f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1u);
    VirtioInput::InputEvent got{};
    EXPECT_FALSE(f.dev->popStatusEvent(got));
}

// 两条队列互不干扰：事件队列照常分发，status 队列独立消费
TEST(VirtioInput, BothQueues) {
    VirtioFixture<VirtioInput> f;
    VirtioInput::InputEvent st{17, 1, 0}; // EV_LED, LED_CAPSL, 灭
    std::memcpy(f.mem() + 0x500, &st, sizeof(st));

    QueueAddrs e{0x100, 0x200, 0x300, 0x400};
    f.putDesc(e.desc, 0, e.data, 16, 2, 0);
    prepareQueue(f, e, 1);
    f.configureQueue(e.desc, e.avail, e.used, 8, 0);

    QueueAddrs s{0x600, 0x700, 0x800, 0x500};
    f.putDesc(s.desc, 0, s.data, 8, 0, 0);
    prepareQueue(f, s, 1);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1);

    ASSERT_TRUE(f.dev->pushInputEvent({1, 30, 1}));
    f.write(0x050, 1);
    for (int i = 0; i < 900; i++)
        f.tick();

    EXPECT_EQ(f.get16(e.used + 2), 1u);
    EXPECT_EQ(f.usedLen(e.used, 0), 8u);
    EXPECT_EQ(f.get16(s.used + 2), 1u);

    VirtioInput::InputEvent got{};
    ASSERT_TRUE(f.dev->popStatusEvent(got));
    EXPECT_EQ(got.code, 1);
}
