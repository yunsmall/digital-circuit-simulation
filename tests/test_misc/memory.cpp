// Memory RAM 测试
#include <dcs/Circuit.h>
#include <dcs/components/memory/Memory.h>
#include <gtest/gtest.h>

TEST(MiscTest, MemoryReadWrite) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto *mem = c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 8));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();

    c.setWire(addr->id(), {3, 0});
    c.setWire(din->id(), {0x42, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    // 同拍读写返回旧值：写入那一拍读到的还是旧数据，下一拍才能读到新值
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42);

    c.setWire(addr->id(), {7, 0});
    c.setWire(din->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xAB);

    c.setWire(addr->id(), {3, 0});
    c.setWire(we->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42);
}

TEST(MiscTest, MemoryNoWriteWithoutWe) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto *mem = c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 8));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();
    c.setWire(addr->id(), {5, 0});
    c.setWire(din->id(), {0xFF, 0});
    c.setWire(we->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0);
}

// 同一拍对同一地址读写：输出写之前的旧值（和 MultiPortRAM / RegFile 一致）
TEST(MiscTest, MemorySameCycleReadGetsOldValue) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto *mem = c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 8));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();

    // 先写 0x42 到地址 3
    c.setWire(addr->id(), {3, 0});
    c.setWire(din->id(), {0x42, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.tick(); // 同拍读写返回旧值，再跑一拍才能读到刚写入的 0x42
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42);

    // 同一拍再写 0x99：这一拍的读输出必须是旧值 0x42
    c.setWire(din->id(), {0x99, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42) << "同拍读写应返回写之前的旧值";

    // 写已经生效，下一拍读到新值
    c.setWire(we->id(), {0, 0});
    c.tick();
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x99);
}
