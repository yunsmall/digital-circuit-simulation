// RingCounter：环形 one-hot 循环、约翰逊 2N 状态、en/rst 行为
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/sequential/RingCounter.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    RingCounter *r = nullptr;
    Net *clk, *en, *rst, *q;

    Fx(int len, bool johnson) {
        auto p = std::make_unique<RingCounter>("rc0", len, johnson);
        r = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        en = c->createNet("en");
        rst = c->createNet("rst");
        q = c->createNet("q");
        c->connect(r, "clk", clk);
        c->connect(r, "en", en);
        c->connect(r, "rst", rst);
        c->connect(r, "q", q);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(clk->id(), {0});
        c->setWire(en->id(), {1});
        c->setWire(rst->id(), {0});
        // 让时序函数先把复位初值写出去，测试里读到的就不是未初始化的输出
        c->tick();
        c->tick();
    }

    uint64_t getQ() {
        auto v = c->getWire(q->id());
        uint64_t out = 0;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            out |= (uint64_t) v[i] << (8 * i);
        return out;
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

TEST(RingCounter, RingRotatesOneHot) {
    Fx f(4, false);
    EXPECT_EQ(f.getQ(), 1u); // init 后未走沿，输出已是初值
    const uint64_t want[4] = {2, 4, 8, 1};
    for (uint64_t w: want) {
        f.edge();
        EXPECT_EQ(f.getQ(), w);
    }
}

TEST(RingCounter, JohnsonHasTwoNStates) {
    Fx f(4, true);
    EXPECT_EQ(f.getQ(), 0u);
    const uint64_t want[8] = {1, 3, 7, 15, 14, 12, 8, 0};
    for (uint64_t w: want) {
        f.edge();
        EXPECT_EQ(f.getQ(), w);
    }
}

TEST(RingCounter, EnableGate) {
    Fx f(4, false);
    f.edge();
    const uint64_t held = f.getQ();
    f.c->setWire(f.en->id(), {0});
    f.edge();
    f.edge();
    EXPECT_EQ(f.getQ(), held);
    f.c->setWire(f.en->id(), {1});
    f.edge();
    EXPECT_NE(f.getQ(), held);
}

TEST(RingCounter, ResetReturnsToStart) {
    Fx f(4, false);
    f.edge();
    f.edge();
    f.c->setWire(f.rst->id(), {1});
    f.edge();
    EXPECT_EQ(f.getQ(), 1u);
    f.c->setWire(f.rst->id(), {0});
    f.edge();
    EXPECT_EQ(f.getQ(), 2u);
}

TEST(RingCounter, JohnsonResetToZero) {
    Fx f(4, true);
    f.edge();
    f.edge();
    f.c->setWire(f.rst->id(), {1});
    f.edge();
    EXPECT_EQ(f.getQ(), 0u);
}

TEST(RingCounter, NoEdgeNoAdvance) {
    Fx f(4, false);
    f.edge();
    const uint64_t q = f.getQ();
    for (int i = 0; i < 5; i++) {
        f.c->setWire(f.clk->id(), {0});
        f.c->tick();
    }
    EXPECT_EQ(f.getQ(), q);
}

TEST(RingCounter, SixtyFourBit) {
    Fx f(64, false);
    f.edge();
    EXPECT_EQ(f.getQ(), 2u);
    f.edge();
    EXPECT_EQ(f.getQ(), 4u);
}
