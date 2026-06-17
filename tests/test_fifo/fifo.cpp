// FIFO — 同步 FIFO 测试
#include <dcs/Circuit.h>
#include <dcs/components/FIFO.h>
#include <gtest/gtest.h>

static void setWireVal(dsc::Circuit &c, int id, uint64_t val, int bw) {
    std::vector<uint8_t> v(16, 0);
    for (int i = 0; i < (bw + 7) / 8; i++)
        v[i] = (val >> (i * 8)) & 0xFF;
    c.setWire(id, v);
}
static void setWireBit(dsc::Circuit &c, int id, bool b) {
    c.setWire(id, {static_cast<uint8_t>(b ? 1 : 0), 0});
}
static bool getWireBit(dsc::Circuit &c, int id) {
    return c.getWire(id)[0] != 0;
}
static uint64_t getWireVal(dsc::Circuit &c, int id, int bw) {
    auto v = c.getWire(id);
    uint64_t r = 0;
    for (int i = 0; i < (bw + 7) / 8 && i < 8; i++)
        r |= (uint64_t) v[i] << (i * 8);
    return r;
}

struct FIFO8x4Setup {
    dsc::Circuit c;
    int din, we, re, clk, rst;
    int dout, full, empty;
    FIFO8x4Setup(bool has_rst = false) {
        din = c.createNet("din")->id();
        we = c.createNet("we")->id();
        re = c.createNet("re")->id();
        clk = c.createNet("clk")->id();
        rst = has_rst ? c.createNet("rst")->id() : -1;
        dout = c.createNet("dout")->id();
        full = c.createNet("full")->id();
        empty = c.createNet("empty")->id();
        auto *f = c.addComponent(std::make_unique<dsc::FIFO>("f", 8, 4, has_rst));
        c.connect(f, "din", c.findNet("din"));
        c.connect(f, "wr_en", c.findNet("we"));
        c.connect(f, "rd_en", c.findNet("re"));
        c.connect(f, "clk", c.findNet("clk"));
        if (has_rst)
            c.connect(f, "rst", c.findNet("rst"));
        c.connect(f, "dout", c.findNet("dout"));
        c.connect(f, "full", c.findNet("full"));
        c.connect(f, "empty", c.findNet("empty"));
        c.compile();
        c.init();
    }
    void tick() {
        setWireBit(c, clk, 0);
        c.tick();
        setWireBit(c, clk, 1);
        c.tick();
    }
};

TEST(FIFO, InitialEmpty) {
    FIFO8x4Setup s;

    // 第一次 tick 驱动输出：空 FIFO，empty=1, full=0
    setWireBit(s.c, s.we, false);
    setWireBit(s.c, s.re, false);
    s.tick();

    EXPECT_TRUE(getWireBit(s.c, s.empty));
    EXPECT_FALSE(getWireBit(s.c, s.full));
}

TEST(FIFO, WriteThenRead) {
    FIFO8x4Setup s;

    // 写入 0x42
    setWireVal(s.c, s.din, 0x42, 8);
    setWireBit(s.c, s.we, true);
    setWireBit(s.c, s.re, false);
    s.tick();

    // 写入后：dout 应显示刚写入的值（读指针还在 0）
    EXPECT_FALSE(getWireBit(s.c, s.empty));
    EXPECT_EQ(getWireVal(s.c, s.dout, 8), 0x42);

    // 读取
    setWireBit(s.c, s.we, false);
    setWireBit(s.c, s.re, true);
    s.tick();

    // 读取后：FIFO 空，dout 显示下一条（空数据）
    EXPECT_TRUE(getWireBit(s.c, s.empty));
}

TEST(FIFO, FullFlag) {
    FIFO8x4Setup s;

    // 写满 4 个数据
    for (int i = 0; i < 4; i++) {
        setWireVal(s.c, s.din, 0x10 + i, 8);
        setWireBit(s.c, s.we, true);
        setWireBit(s.c, s.re, false);
        s.tick();
        // 每次写入后检查 dout（始终指向最早写入的数据）
        EXPECT_EQ(getWireVal(s.c, s.dout, 8), 0x10) << "after write " << i;
    }
    EXPECT_TRUE(getWireBit(s.c, s.full));

    // 逐个读出
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(getWireVal(s.c, s.dout, 8), 0x10 + i) << "before read " << i;
        setWireBit(s.c, s.we, false);
        setWireBit(s.c, s.re, true);
        s.tick();
    }
    EXPECT_TRUE(getWireBit(s.c, s.empty));
}

TEST(FIFO, SimultaneousReadWrite) {
    FIFO8x4Setup s;

    // 先写满
    for (int i = 0; i < 4; i++) {
        setWireVal(s.c, s.din, 0x30 + i, 8);
        setWireBit(s.c, s.we, true);
        setWireBit(s.c, s.re, false);
        s.tick();
    }

    // dout = 0x30（最先写入的）
    EXPECT_EQ(getWireVal(s.c, s.dout, 8), 0x30);

    // 同时读写：写 0x40，读出头部的 0x30
    setWireVal(s.c, s.din, 0x40, 8);
    setWireBit(s.c, s.we, true);
    setWireBit(s.c, s.re, true);
    s.tick();

    // 读后 dout 变为下一个值 0x31（0x30 被读出，0x40 写入队尾）
    EXPECT_EQ(getWireVal(s.c, s.dout, 8), 0x31);
    EXPECT_TRUE(getWireBit(s.c, s.full)); // 仍然满
}

TEST(FIFO, Reset) {
    FIFO8x4Setup s(true);

    // 写入 2 个数据
    for (int i = 0; i < 2; i++) {
        setWireVal(s.c, s.din, 0x50 + i, 8);
        setWireBit(s.c, s.we, true);
        setWireBit(s.c, s.re, false);
        s.tick();
    }
    EXPECT_FALSE(getWireBit(s.c, s.empty));

    // 复位
    setWireBit(s.c, s.rst, 1);
    s.tick();

    EXPECT_TRUE(getWireBit(s.c, s.empty));
    EXPECT_FALSE(getWireBit(s.c, s.full));
}
