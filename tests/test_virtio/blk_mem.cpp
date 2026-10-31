// virtio_blk_mem：介质是设备自己的存储，框架用 IStorage 装载和检查
#include "common.h"

#include <dcs/components/platform/VirtioBlkMem.h>

using namespace dsc;
using namespace dsc::test;

TEST(VirtioBlkMem, ReadFromOwnStorage) {
    VirtioFixture<VirtioBlkMem> f;
    uint8_t *img = f.dev->writableMemPtr();
    ASSERT_NE(img, nullptr);
    for (int i = 0; i < 512; i++)
        img[i] = (uint8_t) (i ^ 0x5A);

    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.get16(used + 2), 1);
    EXPECT_EQ(f.mem()[status], 0);
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(f.mem()[data + i], (uint8_t) (i ^ 0x5A));
}

TEST(VirtioBlkMem, WriteUpdatesOwnStorage) {
    VirtioFixture<VirtioBlkMem> f;
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 1 /*OUT*/, 0, false);
    for (int i = 0; i < 16; i++)
        f.mem()[data + i] = (uint8_t) (0xA0 + i);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 0);
    const uint8_t *img = f.dev->readOnlyMemPtr();
    ASSERT_NE(img, nullptr);
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(img[i], (uint8_t) (0xA0 + i));
}

// 超出介质范围：状态字节必须是 IOERR，不能读到介质以外的宿主内存
TEST(VirtioBlkMem, OutOfRangeReturnsIoError) {
    VirtioFixture<VirtioBlkMem> f;
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 100000, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 1) << "越界读应返回 IOERR";
    EXPECT_EQ(f.get16(used + 2), 1) << "请求仍要正常完成";
}
