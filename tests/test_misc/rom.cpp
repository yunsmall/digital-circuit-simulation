// ROM 测试：hex 字符串 / raw 字节数组 / 文件 mmap
#include <cstdio>
#include <dcs/Circuit.h>
#include <dcs/components/ROM.h>
#include <fstream>
#include <gtest/gtest.h>

// 辅助
static void checkROM(dsc::Circuit &c, int addr_net, int clk_net, int dout_net, int addr, uint8_t expected) {
    c.setWire(addr_net, {(uint8_t) addr, 0});
    c.setWire(clk_net, {0, 0});
    c.tick();
    c.setWire(clk_net, {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout_net)[0], expected);
}

// ============================================================
// hex 字符串（兼容旧版）
// ============================================================
TEST(RomTest, HexString) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 4, 8, "ABCD", 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0xAB);
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0xCD);
}

// ============================================================
// raw 字节数组
// ============================================================
TEST(RomTest, RawData) {
    uint8_t raw[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 3, 8, raw, sizeof(raw), 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0x11);
    checkROM(c, addr->id(), clk->id(), dout->id(), 3, 0x44);
    checkROM(c, addr->id(), clk->id(), dout->id(), 7, 0x88);
}

// ============================================================
// 文件加载（mmap）
// ============================================================
TEST(RomTest, FileMmap) {
    // 写临时 bin 文件（16 字节/行格式，mmap 直接以 addr*16 寻址）
    std::string tmp = std::tmpnam(nullptr);
    tmp += ".bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        // depth=4, 每行 16 字节，数据在每行第 0 字节
        uint8_t row[16] = {};
        row[0] = 0xAA;
        f.write((const char *) row, 16);
        row[0] = 0xBB;
        f.write((const char *) row, 16);
        row[0] = 0xCC;
        f.write((const char *) row, 16);
        row[0] = 0xDD;
        f.write((const char *) row, 16);
    }

    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *clk = c.createNet("clk"), *dout = c.createNet("dout");
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 2, 8, std::filesystem::path(tmp), 0, true));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0xAA);
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0xBB);
    checkROM(c, addr->id(), clk->id(), dout->id(), 2, 0xCC);
    checkROM(c, addr->id(), clk->id(), dout->id(), 3, 0xDD);

    std::remove(tmp.c_str());
}

// ============================================================
// 文件加载（read 模式，不用 mmap）
// ============================================================
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
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 1, 8, std::filesystem::path(tmp), 0, false));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    checkROM(c, addr->id(), clk->id(), dout->id(), 0, 0xFE);
    checkROM(c, addr->id(), clk->id(), dout->id(), 1, 0xED);

    std::remove(tmp.c_str());
}
