// 存储元件的生命周期约定：构造不分配，prepare/可写访问才分配；
// memSize 是逻辑容量，const 访问器不改变状态
#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include <dcs/Circuit.h>
#include <dcs/components/memory/FIFO.h>
#include <dcs/components/memory/Memory.h>
#include <dcs/components/memory/MultiPortRAM.h>
#include <dcs/components/memory/ROM.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/VirtioBlkMem.h>

using namespace dsc;

TEST(StorageLifecycle, Memory) {
    auto mem = std::make_unique<Memory>("m", 4, 8); // 16 字节
    EXPECT_EQ(mem->memSize(), 16u) << "逻辑容量不依赖分配";
    ASSERT_NE(mem->writableMemPtr(), nullptr) << "可写访问应按需分配";
    mem->writableMemPtr()[3] = 0x5A;
    EXPECT_EQ(mem->data()[3], 0x5A);
    EXPECT_NE(mem->readOnlyMemPtr(), nullptr);

    Circuit c;
    auto *added = static_cast<Memory *>(c.addComponent(std::make_unique<Memory>("m2", 8, 16)));
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->memSize(), 256u);
    EXPECT_NE(added->readOnlyMemPtr(), nullptr) << "addComponent 会调用 prepare";
}

TEST(StorageLifecycle, Fifo) {
    auto fifo = std::make_unique<FIFO>("f", 16, 4); // 4 个 16 位字 = 8 字节
    EXPECT_EQ(fifo->memSize(), 8u);
    ASSERT_NE(fifo->writableMemPtr(), nullptr);
    fifo->data()[0] = 0x11;
    EXPECT_EQ(fifo->data()[0], 0x11);
}

TEST(StorageLifecycle, MultiPortRam) {
    auto ram = std::make_unique<MultiPortRAM>("r", 4, 32, 2, 1); // 16 字节
    EXPECT_EQ(ram->memSize(), 16u);
    ASSERT_NE(ram->writableMemPtr(), nullptr);
    ram->writableMemPtr()[15] = 0x7E;
    EXPECT_EQ(ram->readOnlyMemPtr()[15], 0x7E);
}

TEST(StorageLifecycle, WishboneMemory) {
    auto mem = std::make_unique<WishboneMemory>("wm", 6, 32); // 64 字节
    EXPECT_EQ(mem->memSize(), 64u);
    ASSERT_NE(mem->writableMemPtr(), nullptr);
    mem->writableMemPtr()[1] = 0x42;
    EXPECT_EQ(mem->readOnlyMemPtr()[1], 0x42);

    Circuit c;
    auto *added = static_cast<WishboneMemory *>(c.addComponent(std::make_unique<WishboneMemory>("wm2", 4, 8)));
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->memSize(), 16u);
    EXPECT_NE(added->readOnlyMemPtr(), nullptr);
}

TEST(StorageLifecycle, RomMemoryMode) {
    auto rom = std::make_unique<ROM>("r", 4, 8); // 16 字节
    EXPECT_EQ(rom->memSize(), 16u) << "内存模式下容量按参数计算";
    ASSERT_NE(rom->writableMemPtr(), nullptr);
    rom->data()[2] = 0xC3;
    EXPECT_EQ(rom->readOnlyMemPtr()[2], 0xC3);
}

// clone 不再依赖构造函数已分配：先由可写访问分配并写入，clone 必须连数据一起复制
TEST(StorageLifecycle, RomCloneCopiesPreloadedData) {
    ROM rom("r", 4, 8);
    uint8_t *p = rom.writableMemPtr();
    ASSERT_NE(p, nullptr);
    p[0] = 0xAA;
    p[1] = 0xBB;

    auto copy = rom.clone("r2");
    auto *c = static_cast<ROM *>(copy.get());
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->memSize(), 16u);
    EXPECT_EQ(c->data()[0], 0xAA);
    EXPECT_EQ(c->data()[1], 0xBB);
}

TEST(StorageLifecycle, VirtioBlkMem) {
    auto dev = std::make_unique<VirtioBlkMem>("vd", 4096 + 100); // 向下取整到 512 扇区
    EXPECT_EQ(dev->memSize(), 4096u);
    ASSERT_NE(dev->writableMemPtr(), nullptr);
    dev->writableMemPtr()[7] = 0x99;
    EXPECT_EQ(dev->readOnlyMemPtr()[7], 0x99);

    // clone 只记录容量，未 prepare 也能算出正确大小
    auto copy = dev->clone("vd2");
    auto *d = static_cast<VirtioBlkMem *>(copy.get());
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->memSize(), 4096u);
}
