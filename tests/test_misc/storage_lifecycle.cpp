// 存储元件的生命周期约定：构造后处于未就绪状态，只有 addComponent（内部调
// prepare）成功之后才能访问存储；memSize 是逻辑容量，不依赖分配
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>

#include <dcs/Context.h>
#include <dcs/Circuit.h>
#include <dcs/components/memory/FIFO.h>
#include <dcs/components/memory/Memory.h>
#include <dcs/components/memory/MultiPortRAM.h>
#include <dcs/components/memory/ROM.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/VirtioBlkMem.h>

using namespace dsc;

// 所有存储类的共同前置条件：未 prepare 时指针为空、容量可查
TEST(StorageLifecycle, AllStoragesNotReadyBeforePrepare) {
    auto mem = std::make_unique<Memory>("m", 4, 8); // 16 字节
    EXPECT_FALSE(mem->isPrepared());
    EXPECT_EQ(mem->memSize(), 16u) << "逻辑容量只由参数决定";
    EXPECT_EQ(mem->writableMemPtr(), nullptr) << "未就绪不能拿到存储";
    EXPECT_EQ(mem->readOnlyMemPtr(), nullptr);
    EXPECT_EQ(mem->data(), nullptr);

    auto fifo = std::make_unique<FIFO>("f", 16, 4); // 4 个 16 位字 = 8 字节
    EXPECT_FALSE(fifo->isPrepared());
    EXPECT_EQ(fifo->memSize(), 8u);
    EXPECT_EQ(fifo->writableMemPtr(), nullptr);
    EXPECT_EQ(fifo->readOnlyMemPtr(), nullptr);
    EXPECT_EQ(fifo->data(), nullptr);

    auto ram = std::make_unique<MultiPortRAM>("r", 4, 32, 2, 1); // 16 字节
    EXPECT_FALSE(ram->isPrepared());
    EXPECT_EQ(ram->memSize(), 16u);
    EXPECT_EQ(ram->writableMemPtr(), nullptr);
    EXPECT_EQ(ram->readOnlyMemPtr(), nullptr);

    auto wbm = std::make_unique<WishboneMemory>("wm", 6, 32); // 64 字节
    EXPECT_FALSE(wbm->isPrepared());
    EXPECT_EQ(wbm->memSize(), 64u);
    EXPECT_EQ(wbm->writableMemPtr(), nullptr);
    EXPECT_EQ(wbm->readOnlyMemPtr(), nullptr);

    auto rom = std::make_unique<ROM>("r", 4, 8); // 16 字节
    EXPECT_FALSE(rom->isPrepared());
    EXPECT_EQ(rom->memSize(), 16u) << "内存模式下容量按参数计算";
    EXPECT_EQ(rom->readOnlyMemPtr(), nullptr);
    EXPECT_EQ(rom->writableMemPtr(), nullptr);
    EXPECT_EQ(rom->data(), nullptr);

    auto dev = std::make_unique<VirtioBlkMem>("vd", 4096 + 100); // 向下取整到 512 扇区
    EXPECT_FALSE(dev->isPrepared());
    EXPECT_EQ(dev->memSize(), 4096u) << "clone/构造后不分配也能算出容量";
    EXPECT_EQ(dev->readOnlyMemPtr(), nullptr);
    EXPECT_EQ(dev->writableMemPtr(), nullptr);
}

// addComponent 会调用 prepare 并把元件标成就绪
TEST(StorageLifecycle, AddComponentPrepares) {
    Circuit c;
    auto *mem = static_cast<Memory *>(c.addComponent(std::make_unique<Memory>("m", 4, 8)));
    ASSERT_NE(mem, nullptr);
    EXPECT_TRUE(mem->isPrepared());
    ASSERT_NE(mem->writableMemPtr(), nullptr);
    mem->writableMemPtr()[3] = 0x5A;
    EXPECT_EQ(mem->readOnlyMemPtr()[3], 0x5A);
    EXPECT_EQ(mem->data()[3], 0x5A);

    auto *fifo = static_cast<FIFO *>(c.addComponent(std::make_unique<FIFO>("f", 16, 4)));
    ASSERT_NE(fifo, nullptr);
    ASSERT_NE(fifo->writableMemPtr(), nullptr);
    fifo->data()[0] = 0x11;
    EXPECT_EQ(fifo->data()[0], 0x11);

    auto *ram = static_cast<MultiPortRAM *>(c.addComponent(std::make_unique<MultiPortRAM>("r", 4, 32, 2, 1)));
    ASSERT_NE(ram, nullptr);
    ASSERT_NE(ram->writableMemPtr(), nullptr);
    ram->writableMemPtr()[15] = 0x7E;
    EXPECT_EQ(ram->readOnlyMemPtr()[15], 0x7E);

    auto *wbm = static_cast<WishboneMemory *>(c.addComponent(std::make_unique<WishboneMemory>("wm", 6, 32)));
    ASSERT_NE(wbm, nullptr);
    ASSERT_NE(wbm->writableMemPtr(), nullptr);
    wbm->writableMemPtr()[1] = 0x42;
    EXPECT_EQ(wbm->readOnlyMemPtr()[1], 0x42);

    auto *rom = static_cast<ROM *>(c.addComponent(std::make_unique<ROM>("r", 4, 8)));
    ASSERT_NE(rom, nullptr);
    ASSERT_NE(rom->writableMemPtr(), nullptr);
    rom->data()[2] = 0xC3;
    EXPECT_EQ(rom->readOnlyMemPtr()[2], 0xC3);

    auto *dev = static_cast<VirtioBlkMem *>(c.addComponent(std::make_unique<VirtioBlkMem>("vd", 4096)));
    ASSERT_NE(dev, nullptr);
    ASSERT_NE(dev->writableMemPtr(), nullptr);
    dev->writableMemPtr()[7] = 0x99;
    EXPECT_EQ(dev->readOnlyMemPtr()[7], 0x99);
}

// prepare 失败时元件不入电路，也不会被标成就绪
TEST(StorageLifecycle, FailedPrepareStaysNotReady) {
    Context ctx;
    ROM rom("r", 4, 8, std::filesystem::path("dcs_missing_rom_file.bin"));
    std::string err;
    EXPECT_FALSE(rom.prepare(ctx, err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(rom.isPrepared());
    EXPECT_EQ(rom.readOnlyMemPtr(), nullptr);
    EXPECT_EQ(rom.writableMemPtr(), nullptr);
    EXPECT_EQ(rom.memSize(), 0u) << "文件模式未加载时容量未知";
}

// clone 不再依赖构造函数已分配：复制出来的数据要一直保留到加入电路
TEST(StorageLifecycle, RomCloneCopiesPreloadedData) {
    Circuit c;
    auto *rom = static_cast<ROM *>(c.addComponent(std::make_unique<ROM>("r", 4, 8)));
    ASSERT_NE(rom, nullptr);
    rom->writableMemPtr()[0] = 0xAA;
    rom->writableMemPtr()[1] = 0xBB;

    auto copy = rom->clone("r2");
    auto *c2 = static_cast<ROM *>(copy.get());
    ASSERT_NE(c2, nullptr);
    EXPECT_FALSE(c2->isPrepared()) << "clone 出来的元件要重新 prepare";
    EXPECT_EQ(c2->memSize(), 16u);
    EXPECT_EQ(c2->data(), nullptr);

    auto *added = static_cast<ROM *>(c.addComponent(std::move(copy)));
    ASSERT_NE(added, nullptr);
    EXPECT_TRUE(added->isPrepared());
    EXPECT_EQ(added->data()[0], 0xAA);
    EXPECT_EQ(added->data()[1], 0xBB);
}

// VirtioBlkMem 的 clone 只记容量；容量对齐逻辑不受生命周期影响
TEST(StorageLifecycle, VirtioBlkMemCloneKeepsSize) {
    VirtioBlkMem dev("vd", 4096 + 100);
    auto copy = dev.clone("vd2");
    auto *d = static_cast<VirtioBlkMem *>(copy.get());
    ASSERT_NE(d, nullptr);
    EXPECT_FALSE(d->isPrepared());
    EXPECT_EQ(d->memSize(), 4096u);
    EXPECT_EQ(d->writableMemPtr(), nullptr);
}
