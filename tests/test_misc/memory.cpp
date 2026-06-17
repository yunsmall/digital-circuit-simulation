// Memory RAM 测试
#include <dcs/Circuit.h>
#include <dcs/components/Memory.h>
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
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42);

    c.setWire(addr->id(), {7, 0});
    c.setWire(din->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
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
