// virtio_blk_mem：介质是设备自己的存储，框架用 IStorage 装载和检查
#include "common.h"

#include <dcs/ComponentFactory.h>
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

// 扇区偏移超过 4GiB 时不能在 32 位里回绕成 0：那会越过边界检查读到介质开头
TEST(VirtioBlkMem, SectorAbove4GiBIsOutOfRange) {
    VirtioFixture<VirtioBlkMem> f;
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    // 0x800000 * 512 = 2^32，截成 uint32 正好是 0
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, 0x800000, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 1) << "4GiB 以上的扇区不能被当成 0";
    EXPECT_EQ(f.get16(used + 2), 1);
}

// size_kb 为 0 或负数时要在工厂层报出可读错误，不能等到 vector::assign 抛
// 一句实现细节（负数转 uint64 还会回绕成天文数字）
TEST(VirtioBlkMem, InvalidSizeRejected) {
    auto &factory = dsc::ComponentFactory::instance();
    for (const char *bad: {"-1", "0", "999999999"}) {
        std::string err;
        auto comp = factory.create("virtio_blk_mem", "m", {{"size_kb", bad}}, err);
        EXPECT_EQ(comp, nullptr) << bad;
        EXPECT_NE(err.find("size_kb"), std::string::npos) << bad << ": " << err;
    }
}

// sector 是 guest 给的 64 位字段：接近 UINT64_MAX 时乘 512 会回绕成小值，
// 回绕后能通过越界检查，必须按乘加溢出直接返回 IOERR
TEST(VirtioBlkMem, SectorOverflowReturnsIoError) {
    VirtioFixture<VirtioBlkMem> f;
    const uint32_t desc = 0x100, avail = 0x180, used = 0x1C0;
    const uint32_t hdr = 0x200, data = 0x240, status = 0x280;
    f.setupBlkQueue(desc, avail, used, 0, hdr, data, status, 16, 0 /*IN*/, ~0ull, true);
    f.configureQueue(desc, avail, used);
    f.write(0x050, 0);
    for (int i = 0; i < 3000; i++) f.tick();

    EXPECT_EQ(f.mem()[status], 1) << "sector*512 溢出应返回 IOERR";
    EXPECT_EQ(f.get16(used + 2), 1) << "请求仍要正常完成";
}
