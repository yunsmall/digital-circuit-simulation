// RegFile：写后读、同拍读写旧值、越界、多读口、we 门控、复位
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/memory/RegFile.h>

using namespace dsc;

namespace {

void setW(Circuit &c, Net *n, uint32_t v) {
    c.setWire(n->id(), {(uint8_t) v, (uint8_t) (v >> 8)});
}

uint64_t getW(Circuit &c, Net *n) {
    auto v = c.getWire(n->id());
    uint64_t r = 0;
    for (size_t i = 0; i < v.size() && i < 8; i++)
        r |= (uint64_t) v[i] << (8 * i);
    return r;
}

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    RegFile *rf = nullptr;
    Net *clk, *we0, *waddr0, *wdata0, *raddr0, *raddr1, *rdata0, *rdata1;
    int bytes;

    Fx(int bw, int depth) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<RegFile>("rf0", bw, depth, 2, 1);
        rf = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        we0 = c->createNet("we0");
        waddr0 = c->createNet("waddr0");
        wdata0 = c->createNet("wdata0");
        raddr0 = c->createNet("raddr0");
        raddr1 = c->createNet("raddr1");
        rdata0 = c->createNet("rdata0");
        rdata1 = c->createNet("rdata1");
        c->connect(rf, "clk", clk);
        c->connect(rf, "we0", we0);
        c->connect(rf, "waddr0", waddr0);
        c->connect(rf, "wdata0", wdata0);
        c->connect(rf, "raddr0", raddr0);
        c->connect(rf, "raddr1", raddr1);
        c->connect(rf, "rdata0", rdata0);
        c->connect(rf, "rdata1", rdata1);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(clk->id(), 0);
        c->setWire(we0->id(), 0);
        setW(*c, waddr0, 0);
        setW(*c, wdata0, 0);
        setW(*c, raddr0, 0);
        setW(*c, raddr1, 0);
    }

    void edge() {
        c->setWire(clk->id(), 0);
        c->tick();
        c->setWire(clk->id(), 1);
        c->tick();
        c->setWire(clk->id(), 0);
        c->tick();
    }

    void write(uint32_t addr, uint64_t val, bool enable = true) {
        setW(*c, waddr0, addr);
        std::vector<uint8_t> d(bytes);
        for (int i = 0; i < bytes; i++)
            d[i] = (uint8_t) (val >> (8 * i));
        c->setWire(wdata0->id(), d);
        c->setWire(we0->id(), (uint8_t) (enable ? 1 : 0));
        edge();
        c->setWire(we0->id(), 0);
    }

    uint64_t read0(uint32_t addr) {
        setW(*c, raddr0, addr);
        c->tick();
        return getW(*c, rdata0);
    }

    uint64_t read1(uint32_t addr) {
        setW(*c, raddr1, addr);
        c->tick();
        return getW(*c, rdata1);
    }
};

} // namespace

TEST(RegFile, WriteThenRead) {
    Fx f(8, 8);
    f.write(3, 0x5A);
    EXPECT_EQ(f.read0(3), 0x5A);
    EXPECT_EQ(f.read0(0), 0);
}

TEST(RegFile, TwoReadPortsAreIndependent) {
    Fx f(8, 8);
    f.write(1, 0x11);
    f.write(6, 0x66);
    setW(*f.c, f.raddr1, 1);
    f.c->tick();
    EXPECT_EQ(f.read0(6), 0x66);
    EXPECT_EQ(getW(*f.c, f.rdata1), 0x11);
}

TEST(RegFile, ReadBeforeWriteSameCycleSeesOldValue) {
    Fx f(8, 8);
    f.write(2, 0xAA);
    ASSERT_EQ(f.read0(2), 0xAA);
    // 同一拍发起写和读：组合读看到的是写之前的值
    setW(*f.c, f.raddr0, 2);
    setW(*f.c, f.waddr0, 2);
    f.c->setWire(f.wdata0->id(), 0x55);
    f.c->setWire(f.we0->id(), 1);
    f.c->setWire(f.clk->id(), 0);
    f.c->tick();
    EXPECT_EQ(getW(*f.c, f.rdata0), 0xAA);
    f.edge();
    EXPECT_EQ(f.read0(2), 0x55);
}

TEST(RegFile, OutOfRangeReadReturnsZero) {
    Fx f(8, 6); // 深度 6，地址 0~5 有效，6/7 越界
    f.write(5, 0x77);
    EXPECT_EQ(f.read0(5), 0x77);
    EXPECT_EQ(f.read0(6), 0);
    EXPECT_EQ(f.read0(7), 0);
}

TEST(RegFile, OutOfRangeWriteIsIgnored) {
    Fx f(8, 6);
    f.write(6, 0xEE);
    EXPECT_EQ(f.read0(6), 0);
    // 越界写不能把合法内容挤掉
    f.write(4, 0x44);
    EXPECT_EQ(f.read0(4), 0x44);
}

TEST(RegFile, WriteEnableGate) {
    Fx f(8, 8);
    f.write(0, 0x12);
    f.write(0, 0x99, false); // we=0
    EXPECT_EQ(f.read0(0), 0x12);
}

TEST(RegFile, ResetClearsAllEntries) {
    Fx f(8, 8);
    f.write(0, 1);
    f.write(7, 2);
    f.c->reset();
    f.c->init();
    f.c->setWire(f.clk->id(), 0);
    f.c->setWire(f.we0->id(), 0);
    EXPECT_EQ(f.read0(0), 0);
    EXPECT_EQ(f.read0(7), 0);
}

TEST(RegFile, Width64) {
    Fx f(64, 4);
    f.write(1, 0xFEDCBA9876543210ull);
    EXPECT_EQ(f.read0(1), 0xFEDCBA9876543210ull);
}

// rdata → 组合逻辑 → wdata 是寄存器堆最典型的接法（ALU 直接接回写口）。
// 组合依赖只能算 raddr，写口和 clk 是时序路径，否则会被误判成组合环
TEST(RegFile, ReadToWritePathIsNotACombinationalLoop) {
    dsc::Circuit c;
    auto *rf = c.addComponent(std::make_unique<dsc::RegFile>("rf", 8, 4, 1, 1));
    auto *buf = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8, false));
    Net *clk = c.createNet("clk"), *we = c.createNet("we"), *wa = c.createNet("wa");
    Net *wd = c.createNet("wd"), *ra = c.createNet("ra"), *rd = c.createNet("rd");
    c.connect(rf, "clk", clk);
    c.connect(rf, "we0", we);
    c.connect(rf, "waddr0", wa);
    c.connect(rf, "wdata0", wd);
    c.connect(rf, "raddr0", ra);
    c.connect(rf, "rdata0", rd);
    c.connect(buf, "in", rd);
    c.connect(buf, "out", wd);

    auto err = c.compile();
    EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
}
