// Memory 高级测试：C++ 侧访问、写延迟、读延迟
#include <dcs/Circuit.h>
#include <dcs/components/Memory.h>
#include <gtest/gtest.h>

TEST(MiscTest, MemoryCppSideAccess) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto mem = std::make_unique<dsc::Memory>("mem", 4, 8);
    mem->data()[5 * 16] = 0x77;
    mem->data()[10 * 16] = 0x33;
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
    EXPECT_EQ(mem_ptr->data()[5 * 16], 0x99);
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
    EXPECT_EQ(mem->data()[1 * 16], 0);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(busy->id())[0] & 1, 0);
    EXPECT_EQ(mem->data()[1 * 16], 0xAA);
}

TEST(MiscTest, MemoryReadLatency) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din"), *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    auto mem_ptr = std::make_unique<dsc::Memory>("mem", 4, 8, 2, 0);
    mem_ptr->data()[3 * 16] = 0x77;
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
