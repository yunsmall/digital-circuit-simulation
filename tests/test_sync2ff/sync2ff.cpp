// Sync2FF：两级延迟、无时钟沿保持、位宽边界
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/sequential/Sync2FF.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Sync2FF *s = nullptr;
    Net *clk, *d, *q;
    int bytes;

    explicit Fx(int bw) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<Sync2FF>("s0", bw);
        s = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        d = c->createNet("d");
        q = c->createNet("q");
        c->connect(s, "clk", clk);
        c->connect(s, "d", d);
        c->connect(s, "q", q);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(clk->id(), 0);
        setD(0);
    }

    void setD(uint64_t v) {
        std::vector<uint8_t> data(bytes);
        for (int i = 0; i < bytes; i++)
            data[i] = (uint8_t) (v >> (8 * i));
        c->setWire(d->id(), data);
    }

    uint64_t getQ() const {
        auto v = c->getWire(q->id());
        uint64_t r = 0;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            r |= (uint64_t) v[i] << (8 * i);
        return r;
    }

    // 一个完整时钟周期：先低后高，上升沿采样；最后再走一拍低电平，
    // 让时序函数本拍写出的输出传播到线网，后续 getQ 才是这一沿后的值
    void edge() {
        c->setWire(clk->id(), 0);
        c->tick();
        c->setWire(clk->id(), 1);
        c->tick();
        c->setWire(clk->id(), 0);
        c->tick();
    }

    void idle() { c->setWire(clk->id(), 0); }
};

} // namespace

TEST(Sync2FF, OutputLagsInputByTwoEdges) {
    Fx f(8);
    f.setD(0xAA);
    f.edge(); // s1 = AA
    EXPECT_EQ(f.getQ(), 0u);
    f.edge(); // s2 = AA
    EXPECT_EQ(f.getQ(), 0xAAu);
}

TEST(Sync2FF, HoldsWithoutEdge) {
    Fx f(8);
    f.setD(0x11);
    f.edge();
    f.edge();
    ASSERT_EQ(f.getQ(), 0x11u);
    f.setD(0x22);
    // 没有上升沿，输出不能变
    f.c->tick();
    f.c->tick();
    EXPECT_EQ(f.getQ(), 0x11u);
}

TEST(Sync2FF, TracksRepeatedChanges) {
    Fx f(8);
    f.setD(1);
    f.edge();
    f.edge();
    f.setD(2);
    f.edge();
    f.edge();
    f.setD(3);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getQ(), 3u);
}

TEST(Sync2FF, OneBitWidth) {
    Fx f(1);
    f.setD(1);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getQ(), 1u);
}

TEST(Sync2FF, SixtyFourBitWidth) {
    Fx f(64);
    f.setD(0xFEDCBA9876543210ull);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getQ(), 0xFEDCBA9876543210ull);
}

TEST(Sync2FF, MasksBitsAboveWidth) {
    Fx f(4);
    f.setD(0x1F);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getQ(), 0xFu);
}
