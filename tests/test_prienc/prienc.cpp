// PriorityEncoder — 优先编码器测试
#include <dcs/Circuit.h>
#include <dcs/components/PriorityEncoder.h>
#include <format>
#include <gtest/gtest.h>

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

struct PriEnc4Setup {
    dsc::Circuit c;
    int n[4], no, nv;
    PriEnc4Setup() {
        for (int i = 0; i < 4; i++) {
            auto *net = c.createNet(std::format("in{}", i));
            n[i] = net->id();
        }
        no = c.createNet("out")->id();
        nv = c.createNet("valid")->id();
        auto *pe = c.addComponent(std::make_unique<dsc::PriorityEncoder>("pe", 4));
        for (int i = 0; i < 4; i++)
            c.connect(pe, std::format("in{}", i), c.findNet(std::format("in{}", i)));
        c.connect(pe, "out", c.findNet("out"));
        c.connect(pe, "valid", c.findNet("valid"));
        c.compile();
        c.init();
    }
    void run(std::initializer_list<bool> bits) {
        int i = 0;
        for (bool b: bits)
            setWireBit(c, n[i++], b);
        c.tick();
    }
    uint64_t out() {
        return getWireVal(c, no, 2);
    }
    bool valid() {
        return getWireBit(c, nv);
    }
};

TEST(PriorityEncoder, NoInput) {
    PriEnc4Setup s;
    s.run({false, false, false, false});
    EXPECT_FALSE(s.valid());
}

TEST(PriorityEncoder, LowestOnly) {
    PriEnc4Setup s;
    s.run({true, false, false, false}); // 只有 in0=1
    EXPECT_EQ(s.out(), 0);
    EXPECT_TRUE(s.valid());
}

TEST(PriorityEncoder, HighestOnly) {
    PriEnc4Setup s;
    s.run({false, false, false, true}); // 只有 in3=1
    EXPECT_EQ(s.out(), 3);
    EXPECT_TRUE(s.valid());
}

TEST(PriorityEncoder, MultipleInputs) {
    PriEnc4Setup s;
    s.run({true, false, true, false}); // in0=1, in2=1 → 输出最高索引2
    EXPECT_EQ(s.out(), 2);
    EXPECT_TRUE(s.valid());
}

TEST(PriorityEncoder, AllInputs) {
    PriEnc4Setup s;
    s.run({true, true, true, true}); // 全部=1 → 输出最高索引3
    EXPECT_EQ(s.out(), 3);
    EXPECT_TRUE(s.valid());
}

// 8 输入优先编码器
TEST(PriorityEncoder, EightInputs) {
    dsc::Circuit c;
    std::vector<int> nets;
    for (int i = 0; i < 8; i++)
        nets.push_back(c.createNet(std::format("in{}", i))->id());
    int no = c.createNet("out")->id();
    int nv = c.createNet("valid")->id();
    auto *pe = c.addComponent(std::make_unique<dsc::PriorityEncoder>("pe", 8));
    for (int i = 0; i < 8; i++)
        c.connect(pe, std::format("in{}", i), c.findNet(std::format("in{}", i)));
    c.connect(pe, "out", c.findNet("out"));
    c.connect(pe, "valid", c.findNet("valid"));
    c.compile();
    c.init();

    // in5=1 → out=5
    setWireBit(c, nets[5], true);
    c.tick();
    EXPECT_EQ(getWireVal(c, no, 3), 5);
    EXPECT_TRUE(getWireBit(c, nv));

    // in5=1, in7=1 → out=7（最高优先级）
    setWireBit(c, nets[7], true);
    c.tick();
    EXPECT_EQ(getWireVal(c, no, 3), 7);
}
