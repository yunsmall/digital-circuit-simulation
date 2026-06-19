// Memory 高级测试：C++ 侧访问、写延迟、读延迟
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/Memory.h>
#include <gtest/gtest.h>

TEST(MiscTest, MemoryCppSideAccess) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto mem = std::make_unique<dsc::Memory>("mem", 4, 8);
    mem->data()[5 * 1] = 0x77;
    mem->data()[10 * 1] = 0x33;
    auto *mem_ptr = static_cast<dsc::Memory *>(c.addComponent(std::move(mem)));
    c.connect(mem_ptr, "addr", addr);
    c.connect(mem_ptr, "data_in", din);
    c.connect(mem_ptr, "we", we);
    c.connect(mem_ptr, "clk", clk);
    c.connect(mem_ptr, "data_out", dout);
    c.compile();
    c.init();
    c.setWire(addr->id(), {5, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x77);
    c.setWire(addr->id(), {10, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x33);
    c.setWire(addr->id(), {5, 0});
    c.setWire(din->id(), {0x99, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(mem_ptr->data()[5 * 1], 0x99);
}

TEST(MiscTest, MemoryWriteLatency) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout"), *busy = c.createNet("busy");
    auto *mem = static_cast<dsc::Memory *>(c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 8, 0, 2)));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.connect(mem, "busy", busy);
    c.compile();
    c.init();
    c.setWire(addr->id(), {1, 0});
    c.setWire(din->id(), {0xAA, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(we->id(), {0, 0});
    EXPECT_EQ(c.getWire(busy->id())[0] & 1, 1);
    EXPECT_EQ(mem->data()[1 * 1], 0);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(busy->id())[0] & 1, 0);
    EXPECT_EQ(mem->data()[1 * 1], 0xAA);
}

TEST(MiscTest, MemoryReadLatency) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto mem_ptr = std::make_unique<dsc::Memory>("mem", 4, 8, 2, 0);
    mem_ptr->data()[3 * 1] = 0x77;
    auto *mem = c.addComponent(std::move(mem_ptr));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();
    c.setWire(addr->id(), {3, 0});
    c.setWire(we->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0);
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x77);
}

// ============================================================
// 字节寻址：data_width=16 非对齐读写
// ============================================================
TEST(MiscTest, MemoryByteAddr16) {
    // addr_width=3 → 8 字节容量
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto *mem_ptr = static_cast<dsc::Memory*>(c.addComponent(std::make_unique<dsc::Memory>("mem", 3, 16)));
    c.connect(mem_ptr, "addr", addr);
    c.connect(mem_ptr, "data_in", din);
    c.connect(mem_ptr, "we", we);
    c.connect(mem_ptr, "clk", clk);
    c.connect(mem_ptr, "data_out", dout);
    c.compile();
    c.init();

    // 按字节直接写 C++ 侧内存：字节 0-1=0xBBAA, 字节 2-3=0xDDCC
    mem_ptr->data()[0] = 0xAA; mem_ptr->data()[1] = 0xBB;
    mem_ptr->data()[2] = 0xCC; mem_ptr->data()[3] = 0xDD;

    // 读地址 0: 字节 0-1 → 0xBBAA
    c.setWire(addr->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xAA);
    EXPECT_EQ(c.getWire(dout->id())[1], 0xBB);

    // 读地址 1: 字节 1-2 → 0xCCBB（非对齐）
    c.setWire(addr->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xBB);
    EXPECT_EQ(c.getWire(dout->id())[1], 0xCC);

    // 读地址 2: 字节 2-3 → 0xDDCC
    c.setWire(addr->id(), {2, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xCC);
    EXPECT_EQ(c.getWire(dout->id())[1], 0xDD);

    // 读地址 7: 7+2=9>8 → 超出边界 → 0
    c.setWire(addr->id(), {7, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0);
    EXPECT_EQ(c.getWire(dout->id())[1], 0);
}

// ============================================================
// 字节寻址：data_width=32 非对齐读写
// ============================================================
TEST(MiscTest, MemoryByteAddr32) {
    // addr_width=4 → 16 字节容量
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto *mem32 = static_cast<dsc::Memory*>(c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 32)));
    c.connect(mem32, "addr", addr);
    c.connect(mem32, "data_in", din);
    c.connect(mem32, "we", we);
    c.connect(mem32, "clk", clk);
    c.connect(mem32, "data_out", dout);
    c.compile();
    c.init();

    // 写 C++ 侧内存：字节 0-7
    uint8_t raw[] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    std::memcpy(mem32->data(), raw, 8);

    auto read32 = [&](int a) -> uint64_t {
        c.setWire(addr->id(), {(uint8_t)a, (uint8_t)(a >> 8), 0});
        c.tick();
        auto v = c.getWire(dout->id());
        uint64_t val = 0;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            val |= ((uint64_t)v[i]) << (i * 8);
        return val;
    };

    // 地址 0: 字节 0-3 → 0xDDCCBBAA
    EXPECT_EQ(read32(0), 0xDDCCBBAAULL);
    // 地址 1: 字节 1-4 → 0x11DDCCBB
    EXPECT_EQ(read32(1), 0x11DDCCBBULL);
    // 地址 13: 13+4=17>16 → 超出边界 → 0
    EXPECT_EQ(read32(13), 0ULL);
}

// ============================================================
// 字节寻址：写入非对齐 + 读回验证
// ============================================================
TEST(MiscTest, MemoryByteAddrWriteNonAligned) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto *mem16 = static_cast<dsc::Memory*>(c.addComponent(std::make_unique<dsc::Memory>("mem", 3, 16)));
    c.connect(mem16, "addr", addr);
    c.connect(mem16, "data_in", din);
    c.connect(mem16, "we", we);
    c.connect(mem16, "clk", clk);
    c.connect(mem16, "data_out", dout);
    c.compile();
    c.init();

    // 写 0xCDAB 到地址 1（非对齐写）
    c.setWire(addr->id(), {1, 0});
    c.setWire(din->id(), {0xAB, 0xCD, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();

    // 验证 C++ 侧内存
    EXPECT_EQ(mem16->data()[1], 0xAB);
    EXPECT_EQ(mem16->data()[2], 0xCD);
    // 相邻字节不受影响
    EXPECT_EQ(mem16->data()[0], 0);
    EXPECT_EQ(mem16->data()[3], 0);

    // 地址 7: 7+2=9>8 → 超出边界，写失败
    c.setWire(addr->id(), {7, 0});
    c.setWire(din->id(), {0xFF, 0xFF, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(mem16->data()[7], 0); // 没写进去
}
