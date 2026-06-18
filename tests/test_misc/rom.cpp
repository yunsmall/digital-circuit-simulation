// ROM 测试：内存模式 / 文件 mmap / 文件 read
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

// 辅助：读 ROM 输出
static void checkROM(dsc::Circuit &c, int addr_net, int clk_net, int dout_net, int addr, uint8_t expected) {
    c.setWire(addr_net, {(uint8_t) addr, 0});
    c.setWire(clk_net, {0, 0});
    c.tick();
    c.setWire(clk_net, {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout_net)[0], expected);
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
