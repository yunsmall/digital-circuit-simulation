// ROM 测试：内存模式 / 文件 mmap / 文件 read / 字节寻址非对齐访问
#include <cstdio>
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/ROM.h>
#include <fstream>
#include <gtest/gtest.h>

// 辅助：给 ROM 的内存模式写入二进制数据
static void writeROM(dsc::ROM *rom, const uint8_t *data, size_t len) {
    size_t n = std::min(len, rom->memSize());
    std::memcpy(rom->data(), data, n);
}

// 辅助：逐字节比较输出（data_width <= 8 的简单场景）
static void checkROM(dsc::Circuit &c, int addr_net, int clk_net, int dout_net, int addr, uint8_t expected) {
    c.setWire(addr_net, {(uint8_t) addr, 0});
    c.setWire(clk_net, {0, 0});
    c.tick();
    c.setWire(clk_net, {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout_net)[0], expected);
}

// 辅助：读多字节输出，返回 uint64_t（小端）
static uint64_t readROM(dsc::Circuit &c, int addr_net, int clk_net, int dout_net, int addr) {
    c.setWire(addr_net, {(uint8_t)addr, (uint8_t)(addr >> 8), 0});
    c.setWire(clk_net, {0, 0});
    c.tick();
    c.setWire(clk_net, {1, 0});
    c.tick();
    auto v = c.getWire(dout_net);
    uint64_t val = 0;
    for (size_t i = 0; i < v.size() && i < 8; i++)
        val |= ((uint64_t)v[i]) << (i * 8);
    return val;
}

// ============================================================
// 内存模式：通过 IStorage 写入二进制数据
// ============================================================
TEST(RomTest, MemoryMode) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 4, 8, 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);

    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    writeROM(static_cast<dsc::ROM *>(rom), data, sizeof(data));

    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0xAA);
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0xBB);
    checkROM(c, addr->id(), clk->id(), dout->id(), 2, 0xCC);
    checkROM(c, addr->id(), clk->id(), dout->id(), 3, 0xDD);
}

// ============================================================
// 文件加载（mmap）
// ============================================================
TEST(RomTest, FileMmap) {
    // 紧凑原始字节文件，depth=4（addr_width=2），文件只有 2 字节
    std::string tmp = std::tmpnam(nullptr);
    tmp += ".bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        uint8_t data[] = {0xAA, 0xBB};
        f.write((const char *) data, sizeof(data));
    }

    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 2, 8, std::filesystem::path(tmp), 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    // 文件范围内：地址 0=0xAA, 地址 1=0xBB
    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0xAA);
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0xBB);
    // 超出文件范围：地址 2 和 3 应返回 0
    checkROM(c, addr->id(), clk->id(), dout->id(), 2, 0);
    checkROM(c, addr->id(), clk->id(), dout->id(), 3, 0);

    std::remove(tmp.c_str());
}

// ============================================================
// 文件加载（read 模式，不用 mmap）
// ============================================================
// 文件加载，文件刚好等于 depth
TEST(RomTest, FileRead) {
    std::string tmp = std::tmpnam(nullptr);
    tmp += ".bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        uint8_t data[] = {0xFE, 0xED};
        f.write((const char *) data, sizeof(data));
    }

    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 1, 8, std::filesystem::path(tmp), 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0xFE);
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0xED);

    std::remove(tmp.c_str());
}

// 文件小于 ROM 深度，超出部分返回 0（read 模式）
TEST(RomTest, FileReadSmall) {
    std::string tmp = std::tmpnam(nullptr);
    tmp += ".bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        uint8_t data[] = {0x11};
        f.write((const char *) data, sizeof(data));
    }

    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    // depth=4（addr_width=2），文件只有 1 字节
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 2, 8, std::filesystem::path(tmp), 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0x11); // 文件范围内
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0);    // 超出文件，返回 0
    checkROM(c, addr->id(), clk->id(), dout->id(), 2, 0);
    checkROM(c, addr->id(), clk->id(), dout->id(), 3, 0);

    std::remove(tmp.c_str());
}

// ============================================================
// 字节寻址：data_width=32 非对齐读取
// ============================================================
TEST(RomTest, ByteAddr32) {
    // addr_width=4 → 16 字节容量
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 4, 32, 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);

    // 写入 8 字节数据
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    writeROM(static_cast<dsc::ROM *>(rom), data, sizeof(data));

    c.compile();
    c.init();

    // 地址 0: 字节 0-3 → 0xDDCCBBAA
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 0), 0xDDCCBBAAULL);
    // 地址 1: 字节 1-4 → 0x11DDCCBB
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 1), 0x11DDCCBBULL);
    // 地址 2: 字节 2-5 → 0x2211DDCC
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 2), 0x2211DDCCULL);
    // 地址 14: 14+4=18>16 → 超出边界 → 0
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 14), 0ULL);
}

// ============================================================
// 字节寻址：data_width=16 边界测试
// ============================================================
TEST(RomTest, ByteAddr16) {
    // addr_width=3 → 8 字节容量
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 3, 16, 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);

    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    writeROM(static_cast<dsc::ROM *>(rom), data, sizeof(data));

    c.compile();
    c.init();

    // 地址 0: 字节 0-1 → 0xBBAA
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 0), 0xBBAAULL);
    // 地址 1: 字节 1-2 → 0xCCBB（非对齐）
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 1), 0xCCBBULL);
    // 地址 6: 字节 6-7 → 0x4433
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 6), 0x4433ULL);
    // 地址 7: 7+2=9>8 → 超出边界 → 0
    EXPECT_EQ(readROM(c, addr->id(), clk->id(), dout->id(), 7), 0ULL);
}
