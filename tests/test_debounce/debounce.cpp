// Debounce：稳定拍数阈值、回弹清零、位宽边界
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/sequential/Debounce.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Debounce *d = nullptr;
    Net *clk, *in, *out;
    int bytes;

    Fx(int bw, int stable) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<Debounce>("db0", bw, stable);
        d = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        in = c->createNet("in");
        out = c->createNet("out");
        c->connect(d, "clk", clk);
        c->connect(d, "in", in);
        c->connect(d, "out", out);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(clk->id(), {0});
        setIn(0);
    }

    void setIn(uint64_t v) {
        std::vector<uint8_t> data(bytes);
        for (int i = 0; i < bytes; i++)
            data[i] = (uint8_t) (v >> (8 * i));
        c->setWire(in->id(), data);
    }

    uint64_t getOut() const {
        auto v = c->getWire(out->id());
        uint64_t r = 0;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            r |= (uint64_t) v[i] << (8 * i);
        return r;
    }

    void edge() {
        c->setWire(clk->id(), {0});
        c->tick();
        c->setWire(clk->id(), {1});
        c->tick();
        c->setWire(clk->id(), {0});
        c->tick();
    }
};

} // namespace

TEST(Debounce, NeedsStableEdgesBeforeUpdating) {
    Fx f(8, 3);
    f.setIn(1);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getOut(), 0u); // 只有两次沿，还不够
    f.edge();
    EXPECT_EQ(f.getOut(), 1u);
}

TEST(Debounce, BounceResetsCounter) {
    Fx f(8, 3);
    f.setIn(1);
    f.edge();
    f.edge();
    f.setIn(0); // 回弹，计数清零
    f.edge();
    f.setIn(1);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getOut(), 0u);
    f.edge();
    EXPECT_EQ(f.getOut(), 1u);
}

TEST(Debounce, SingleEdgeThreshold) {
    Fx f(8, 1);
    f.setIn(5);
    f.edge();
    EXPECT_EQ(f.getOut(), 5u);
}

TEST(Debounce, IgnoresNonEdgeTicks) {
    Fx f(8, 2);
    f.setIn(1);
    // 一直保持低电平，没有上升沿就不该计数
    f.c->setWire(f.clk->id(), {0});
    for (int i = 0; i < 10; i++)
        f.c->tick();
    EXPECT_EQ(f.getOut(), 0u);
}

TEST(Debounce, MultiBitWidthTracksWholeBus) {
    Fx f(8, 2);
    f.setIn(0xA5);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getOut(), 0xA5u);
    f.setIn(0x5A);
    f.edge();
    f.edge();
    EXPECT_EQ(f.getOut(), 0x5Au);
}

TEST(Debounce, MasksBitsAboveWidth) {
    Fx f(4, 1);
    f.setIn(0x1F);
    f.edge();
    EXPECT_EQ(f.getOut(), 0xFu);
}
