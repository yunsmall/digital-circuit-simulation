// virtio-net：队列 0 收包、队列 1 发包
#include "common.h"

#include <dcs/components/platform/VirtioNet.h>

using namespace dsc;
using namespace dsc::test;

// 驱动提交可写 buffer，宿主塞帧，设备写 12 字节头 + 帧体
TEST(VirtioNet, ReceivePacket) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0); // F_WRITE
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 0); // RX 是队列 0
    f.write(0x070, 1);
    f.write(0x050, 0);

    uint8_t pkt[16];
    for (int i = 0; i < 16; i++) pkt[i] = (uint8_t) (0xA0 + i);
    ASSERT_TRUE(f.dev->pushRxPacket(pkt, 16));
    for (int i = 0; i < 900; i++) f.tick();

    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 28u); // 12 字节头 + 16 字节帧
    for (int i = 0; i < 12; i++)
        EXPECT_EQ(f.mem()[s.data + i], 0) << "virtio_net_hdr 应全 0";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[s.data + 12 + i], (uint8_t) (0xA0 + i));
}

// buffer 装不下 12 字节头：以 len=0 完成，包留在环里等下一个 buffer
TEST(VirtioNet, TinyRxBufferKeepsPacket) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 8, 2, 0); // 比 virtio_net_hdr 还小
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 0);
    f.write(0x070, 1);
    f.write(0x050, 0);

    uint8_t pkt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(f.dev->pushRxPacket(pkt, 8));
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 0u);
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(f.mem()[s.data + i], 0) << "buffer 太小，设备不该写入";

    // 换一个够大的 buffer，包还在，应该被写进去
    f.putDesc(s.desc, 1, s.data + 32, 64, 2, 0);
    f.put16(s.avail + 2, 2);
    f.put16(s.avail + 6, 1);
    f.write(0x050, 0);
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 2);
    EXPECT_EQ(f.usedLen(s.used, 1), 20u); // 12 + 8
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(f.mem()[s.data + 32 + 12 + i], pkt[i]);
}

// 发包：驱动提交 12 字节头 + 帧体，设备读出来交给宿主
TEST(VirtioNet, TransmitPacket) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 20, 0, 0); // 可读
    std::memset(f.mem() + s.data, 0, 12);
    for (int i = 0; i < 8; i++)
        f.mem()[s.data + 12 + i] = (uint8_t) (i + 1);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1); // TX 是队列 1
    f.write(0x070, 1);
    f.write(0x050, 1);

    for (int i = 0; i < 900; i++) f.tick();

    uint8_t out[64];
    const uint32_t n = f.dev->popTxPacket(out, sizeof(out));
    EXPECT_EQ(n, 8u) << "宿主应该拿到去掉 12 字节头的帧体";
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], (uint8_t) (i + 1));
    EXPECT_EQ(f.get16(s.used + 2), 1);
    EXPECT_EQ(f.usedLen(s.used, 0), 8u);
}

// 链式发包：头和数据在不同的描述符里
TEST(VirtioNet, TransmitChained) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 12, 3, 1);     // NEXT，只有头
    f.putDesc(s.desc, 1, s.data + 16, 6, 0, 0); // 帧体
    f.put16(s.data, 0);                         // 头内容
    for (int i = 0; i < 6; i++)
        f.mem()[s.data + 16 + i] = (uint8_t) (0x10 + i);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 1);
    f.write(0x070, 1);
    f.write(0x050, 1);

    for (int i = 0; i < 900; i++) f.tick();

    uint8_t out[64];
    const uint32_t n = f.dev->popTxPacket(out, sizeof(out));
    EXPECT_EQ(n, 6u);
    for (int i = 0; i < 6; i++)
        EXPECT_EQ(out[i], (uint8_t) (0x10 + i));
    EXPECT_EQ(f.usedLen(s.used, 0), 6u);
}

// 队列没 ready 时两个队列都不动
TEST(VirtioNet, IgnoresWhenNotReady) {
    VirtioFixture<VirtioNet> f;
    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.write(0x030, 0);
    f.write(0x038, 8);
    f.write(0x080, s.desc);
    f.write(0x090, s.avail);
    f.write(0x0A0, s.used);
    f.write(0x070, 1);
    f.write(0x050, 0);

    uint8_t pkt[4] = {1, 2, 3, 4};
    ASSERT_TRUE(f.dev->pushRxPacket(pkt, 4));
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.get16(s.used + 2), 0u);
}

// RX 宿主环满时 push 返回 false；设备消费后又能继续
TEST(VirtioNet, RxRingFull) {
    VirtioFixture<VirtioNet> f;
    uint8_t pkt[4] = {1, 2, 3, 4};
    int pushed = 0;
    while (f.dev->pushRxPacket(pkt, 4))
        pushed++;
    EXPECT_EQ(pushed, 7); // 环容量 8，留一个空位

    QueueAddrs s{0x100, 0x200, 0x300, 0x400};
    f.putDesc(s.desc, 0, s.data, 64, 2, 0);
    prepareQueue(f, s, 1);
    f.put16(s.avail + 4, 0);
    f.configureQueue(s.desc, s.avail, s.used, 8, 0);
    f.write(0x070, 1);
    f.write(0x050, 0);
    for (int i = 0; i < 600; i++) f.tick();
    EXPECT_EQ(f.usedLen(s.used, 0), 16u);
    EXPECT_TRUE(f.dev->pushRxPacket(pkt, 4));
}
