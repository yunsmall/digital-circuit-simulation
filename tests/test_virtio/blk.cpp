// virtio-blk：寄存器、队列握手、镜像读写
#include "common.h"

#include <dcs/components/platform/VirtioBlk.h>

using namespace dsc;
using namespace dsc::test;

TEST(VirtioTest, MagicVersionAndDeviceId) {
    VirtioFixture<VirtioBlk> f;
    EXPECT_EQ(f.read(0x000), 0x74726976u) << "magic 应为 \"virt\"";
    EXPECT_EQ(f.read(0x004), 2u) << "只实现 modern（version 2）";
    EXPECT_EQ(f.read(0x008), 2u) << "blk 的 device id 是 2";
    EXPECT_EQ(f.read(0x00C), 0x444353u);
}

TEST(VirtioTest, FeatureSelectors) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x014, 0); // sel 0
    EXPECT_EQ(f.read(0x010), 0u) << "没有额外特性";
    f.write(0x014, 1); // sel 1
    EXPECT_EQ(f.read(0x010), 1u) << "sel1 必须声明 VIRTIO_F_VERSION_1";
}

// QueueNumMax 太小会让 Linux 的 virtio-net 判定环永远不够用：
// num_free < 2 + MAX_SKB_FRAGS 时驱动直接停掉 TX 队列，发包后不再 notify
TEST(VirtioTest, QueueNumMaxIsUsable) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x030, 0); // queue selector
    EXPECT_GE(f.read(0x034), 64u);
}

TEST(VirtioTest, ReadFromBackendImage) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    for (int i = 0; i < 512; i++)
        f.image[i] = (uint8_t) i;

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0); // notify
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1) << "请求完成后 used.idx 应加一";
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) i) << "第 " << i << " 字节不匹配";
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 1) << "完成请求后应拉中断";
}

// 非对齐 buffer：主端口必须按字内剩余空间拆事务，SEL 不能跨出 4 位
TEST(VirtioTest, UnalignedDataBuffer) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    for (int i = 0; i < 512; i++)
        f.image[i] = (uint8_t) (0x40 + i);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x241, status = 0x300;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 4000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1);
    EXPECT_EQ(f.mem()[status], 0) << "状态字节应为 OK";
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) (0x40 + i)) << "第 " << i << " 字节不匹配";
}

TEST(VirtioTest, WriteToReadOnlyImageReturnsRoError) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadOnly);
    EXPECT_NE(f.read(0x010) & (1u << 5), 0u) << "只读后端应声明 VIRTIO_BLK_F_RO";

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    std::memset(f.mem() + data, 0xAA, 16);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 3) << "对只读后端写入应返回 RO 错误";
    EXPECT_EQ(f.image[0], 0) << "镜像不能被改动";
}

TEST(VirtioTest, UnsupportedRequestReturnsUnsupp) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 99, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.mem()[status], 2) << "未知请求类型应返回 UNSUPP";
}

TEST(VirtioTest, InterruptAckClearsLine) {
    VirtioFixture<VirtioBlk> f(BlkImage::ReadWrite);
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    f.setupBlkQueue(desc, avail, used, 0, 0x200, 0x240, 0x280, 16, 0, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();
    EXPECT_EQ(f.read(0x060) & 1u, 1u) << "interrupt_status 应置 VRING 位";
    f.write(0x064, 1); // ack
    for (int i = 0; i < 2; i++) f.tick();
    EXPECT_EQ(f.c->getWire(f.irq->id())[0], 0) << "ack 后中断线应撤销";
}

TEST(VirtioTest, StatusZeroResetsQueue) {
    VirtioFixture<VirtioBlk> f;
    f.write(0x030, 0);
    f.write(0x038, 4);
    f.write(0x044, 1);
    f.write(0x070, 0);
    f.write(0x030, 0);
    EXPECT_EQ(f.read(0x044), 0u) << "status 写 0 应复位队列";
}
