// 测试杂项元件: Constant, Switch, Merge, Split, Memory + 环路检测
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Memory.h>
#include <dcs/components/Misc.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

// ============================================================
// Constant — 常量输出
// ============================================================
TEST(MiscTest, Constant8) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    c.addComponent(std::make_unique<dsc::Constant>("c", 8, 0xAB));
    c.connect(c.components()[0].get(), "out", o);
    c.compile();
    c.init();
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xAB);
}

TEST(MiscTest, Constant32) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    c.addComponent(std::make_unique<dsc::Constant>("c", 32, 0xDEADBEEF));
    c.connect(c.components()[0].get(), "out", o);
    c.compile();
    c.init();
    c.tick();
    auto v = c.getWire(o->id());
    EXPECT_EQ(v[0], 0xEF);
    EXPECT_EQ(v[1], 0xBE);
    EXPECT_EQ(v[2], 0xAD);
    EXPECT_EQ(v[3], 0xDE);
}

// ============================================================
// Switch — 使能导通
// ============================================================
TEST(MiscTest, SwitchOn) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *en = c.createNet("en"), *o = c.createNet("out");
    auto *sw = c.addComponent(std::make_unique<dsc::Switch>("sw", 8));
    c.connect(sw, "in", in);
    c.connect(sw, "en", en);
    c.connect(sw, "out", o);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(en->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xAB);
}

TEST(MiscTest, SwitchOff) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *en = c.createNet("en"), *o = c.createNet("out");
    auto *sw = c.addComponent(std::make_unique<dsc::Switch>("sw", 8));
    c.connect(sw, "in", in);
    c.connect(sw, "en", en);
    c.connect(sw, "out", o);
    c.compile();
    c.init();
    c.setWire(in->id(), {0xAB, 0});
    c.setWire(en->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0);
}

// ============================================================
// Merge — 多位 1-bit 合并
// ============================================================
TEST(MiscTest, Merge4) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1");
    auto *i2 = c.createNet("in2"), *i3 = c.createNet("in3");
    auto *o = c.createNet("out");
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 4));
    c.connect(mg, "in0", i0);
    c.connect(mg, "in1", i1);
    c.connect(mg, "in2", i2);
    c.connect(mg, "in3", i3);
    c.connect(mg, "out", o);
    c.compile();
    c.init();

    // 0b1010 = 0xA
    c.setWire(i0->id(), {0, 0});
    c.setWire(i1->id(), {1, 0});
    c.setWire(i2->id(), {0, 0});
    c.setWire(i3->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0x0A); // 0b1010
}

TEST(MiscTest, Merge8) {
    dsc::Circuit c;
    auto *o = c.createNet("out");
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 8));
    std::vector<dsc::Net *> ins;
    for (int i = 0; i < 8; i++) {
        ins.push_back(c.createNet(std::format("in{}", i)));
        c.connect(mg, std::format("in{}", i), ins[i]);
    }
    c.connect(mg, "out", o);
    c.compile();
    c.init();

    // 0b10110001 = 0xB1
    uint8_t bits[8] = {1, 0, 0, 0, 1, 1, 0, 1};
    for (int i = 0; i < 8; i++)
        c.setWire(ins[i]->id(), {bits[i], 0});
    c.tick();
    EXPECT_EQ(c.getWire(o->id())[0], 0xB1);
}

// ============================================================
// Split — 总线拆分
// ============================================================
TEST(MiscTest, Split4) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *o0 = c.createNet("out0");
    auto *o1 = c.createNet("out1"), *o2 = c.createNet("out2"), *o3 = c.createNet("out3");
    auto *sp = c.addComponent(std::make_unique<dsc::Split>("sp", 4));
    c.connect(sp, "in", in);
    c.connect(sp, "out0", o0);
    c.connect(sp, "out1", o1);
    c.connect(sp, "out2", o2);
    c.connect(sp, "out3", o3);
    c.compile();
    c.init();

    // 0b1010
    c.setWire(in->id(), {0x0A, 0});
    c.tick();
    EXPECT_EQ(c.getWire(o0->id())[0] & 1, 0);
    EXPECT_EQ(c.getWire(o1->id())[0] & 1, 1);
    EXPECT_EQ(c.getWire(o2->id())[0] & 1, 0);
    EXPECT_EQ(c.getWire(o3->id())[0] & 1, 1);
}

TEST(MiscTest, SplitMergeRoundtrip) {
    dsc::Circuit c;
    auto *in = c.createNet("in"), *bus = c.createNet("bus"), *out = c.createNet("out");
    auto *sp = c.addComponent(std::make_unique<dsc::Split>("sp", 8));
    auto *mg = c.addComponent(std::make_unique<dsc::Merge>("mg", 8));
    c.connect(sp, "in", in);
    for (int i = 0; i < 8; i++)
        c.connect(sp, std::format("out{}", i), c.createNet(std::format("b{}", i)));
    for (int i = 0; i < 8; i++)
        c.connect(mg, std::format("in{}", i), c.nets()[3 + i].get());
    c.connect(mg, "out", out);

    c.compile();
    c.init();
    c.setWire(in->id(), {0xA5, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0xA5);
}

// ============================================================
// Memory — RAM
// ============================================================
TEST(MiscTest, MemoryReadWrite) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din");
    auto *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");

    auto *mem = c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 8)); // 16×8
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();

    // 写入 0x42 到地址 3
    c.setWire(addr->id(), {3, 0});
    c.setWire(din->id(), {0x42, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick(); // clk=0
    c.setWire(clk->id(), {1, 0});
    c.tick(); // 上升沿写入
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42);

    // 写入 0xAB 到地址 7
    c.setWire(addr->id(), {7, 0});
    c.setWire(din->id(), {0xAB, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xAB);

    // 读回地址 3（不使能写）
    c.setWire(addr->id(), {3, 0});
    c.setWire(we->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x42);
}

TEST(MiscTest, MemoryNoWriteWithoutWe) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din");
    auto *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");

    auto *mem = c.addComponent(std::make_unique<dsc::Memory>("mem", 4, 8));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();

    // 初始值为 0
    c.setWire(addr->id(), {5, 0});
    c.setWire(din->id(), {0xFF, 0});
    c.setWire(we->id(), {0, 0}); // 不使能写
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick(); // 不应写入
    EXPECT_EQ(c.getWire(dout->id())[0], 0);
}

TEST(MiscTest, MemoryCppSideAccess) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din");
    auto *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");

    auto mem = std::make_unique<dsc::Memory>("mem", 4, 8);
    // C++ 侧通过裸指针直接预填数据（每个地址占 16 字节）
    mem->data()[5 * 16] = 0x77;
    mem->data()[10 * 16] = 0x33;
    EXPECT_EQ(mem->data()[5 * 16], 0x77);
    EXPECT_EQ(mem->data()[10 * 16], 0x33);

    auto *mem_ptr = static_cast<dsc::Memory *>(c.addComponent(std::move(mem)));
    c.connect(mem_ptr, "addr", addr);
    c.connect(mem_ptr, "data_in", din);
    c.connect(mem_ptr, "we", we);
    c.connect(mem_ptr, "clk", clk);
    c.connect(mem_ptr, "data_out", dout);
    c.compile();
    c.init();

    // JIT 侧读预填数据
    c.setWire(addr->id(), {5, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x77);
    c.setWire(addr->id(), {10, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x33);

    // JIT 侧写入后，C++ 侧通过指针读出
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
    auto *addr = c.createNet("addr"), *din = c.createNet("din");
    auto *we = c.createNet("we"), *clk = c.createNet("clk");
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

    // 写操作
    c.setWire(addr->id(), {1, 0});
    c.setWire(din->id(), {0xAA, 0});
    c.setWire(we->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick(); // 上升沿，捕获到写流水线首级
    c.setWire(we->id(), {0, 0}); // 关 we，避免重复捕获

    // busy=1，数据未写入
    EXPECT_EQ(c.getWire(busy->id())[0] & 1, 1);
    EXPECT_EQ(mem->data()[1 * 16], 0);

    // 等 2 个周期让写延迟到期
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    c.setWire(clk->id(), {0, 0});
    c.tick();

    EXPECT_EQ(c.getWire(busy->id())[0] & 1, 0); // busy 清零
    EXPECT_EQ(mem->data()[1 * 16], 0xAA); // 已写入
}

TEST(MiscTest, MemoryReadLatency) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr"), *din = c.createNet("din");
    auto *we = c.createNet("we"), *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");

    // 预填内存
    auto mem_ptr = std::make_unique<dsc::Memory>("mem", 4, 8, 2, 0); // 读延迟=2
    mem_ptr->data()[3 * 16] = 0x77;
    auto *mem = c.addComponent(std::move(mem_ptr));
    c.connect(mem, "addr", addr);
    c.connect(mem, "data_in", din);
    c.connect(mem, "we", we);
    c.connect(mem, "clk", clk);
    c.connect(mem, "data_out", dout);
    c.compile();
    c.init();

    // 地址=3，但 data_out 还没更新（读延迟=2）
    c.setWire(addr->id(), {3, 0});
    c.setWire(we->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0); // 第1周期：旧数据（0）

    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0); // 第2周期：还在延迟中

    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0x77); // 第3周期：数据有效
}

// ============================================================
// 环路检测
// ============================================================
TEST(ErrorTest, CombinationalLoop) {
    dsc::Circuit c;
    auto *w0 = c.createNet("w0"), *w1 = c.createNet("w1");
    // NOT: w0 → NOT → w1
    auto *g1 = c.addComponent(std::make_unique<dsc::GateNOT>("g1", 8));
    c.connect(g1, "in", w0);
    c.connect(g1, "out", w1);
    // NOT: w1 → NOT → w0  —— 形成环路！
    auto *g2 = c.addComponent(std::make_unique<dsc::GateNOT>("g2", 8));
    c.connect(g2, "in", w1);
    c.connect(g2, "out", w0);
    EXPECT_THROW(c.compile(), std::runtime_error);
    try {
        c.compile();
    } catch (const std::runtime_error &e) {
        EXPECT_TRUE(std::string(e.what()).find("环路") != std::string::npos);
    }
}

TEST(ErrorTest, SequentialBreaksLoop) {
    dsc::Circuit c;
    auto *w0 = c.createNet("w0"), *w1 = c.createNet("w1");
    auto *clk = c.createNet("clk"), *q = c.createNet("q");

    // DFF: w0 → D → Q → w1（时序元件打破环路）
    auto *dff = c.addComponent(std::make_unique<dsc::DFlipFlop>("dff", 8));
    c.connect(dff, "d", w0);
    c.connect(dff, "clk", clk);
    c.connect(dff, "q", w1);
    // NOT: w1 → NOT → w0
    auto *not_g = c.addComponent(std::make_unique<dsc::GateNOT>("not", 8));
    c.connect(not_g, "in", w1);
    c.connect(not_g, "out", w0);

    // DFF 打破了组合环路，不应报错
    EXPECT_NO_THROW(c.compile());
}

TEST(ErrorTest, MultipleDrivers) {
    dsc::Circuit c;
    auto *w = c.createNet("w");
    auto *g1 = c.addComponent(std::make_unique<dsc::GateBUF>("g1", 8));
    c.connect(g1, "in", c.createNet("a"));
    c.connect(g1, "out", w);
    // 第二个输出驱动同一线网 → 应报错
    auto *g2 = c.addComponent(std::make_unique<dsc::GateBUF>("g2", 8));
    c.connect(g2, "in", c.createNet("b"));
    EXPECT_THROW(c.connect(g2, "out", w), std::runtime_error);
}

// ROM 测试
TEST(MiscTest, ROM) {
    dsc::Circuit c;
    auto *addr = c.createNet("addr");
    auto *clk = c.createNet("clk");
    auto *dout = c.createNet("dout");
    // 初始化 ROM: 地址0=0xAB, 地址1=0xCD
    auto *rom = c.addComponent(std::make_unique<dsc::ROM>("rom", 4, 8, "ABCD", 0));
    c.connect(rom, "addr", addr);
    c.connect(rom, "clk", clk);
    c.connect(rom, "data_out", dout);
    c.compile();
    c.init();

    // 读地址 0 -> 0xAB
    c.setWire(addr->id(), {0, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xAB);

    // 读地址 1 -> 0xCD
    c.setWire(addr->id(), {1, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(dout->id())[0], 0xCD);
}
