// Assert：失败计数、条件为真不触发、无时钟沿不触发、打印上限不影响计数
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/platform/Assert.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Assert *a = nullptr;
    Net *clk, *cond;

    explicit Fx(int max_reports = 1) {
        auto p = std::make_unique<Assert>("as0", "cond must hold", max_reports);
        a = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        cond = c->createNet("cond");
        c->connect(a, "clk", clk);
        c->connect(a, "cond", cond);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(clk->id(), {0});
        c->setWire(cond->id(), {1});
    }

    void edge(bool cond_val) {
        c->setWire(cond->id(), {(uint8_t) (cond_val ? 1 : 0)});
        c->setWire(clk->id(), {0});
        c->tick();
        c->setWire(clk->id(), {1});
        c->tick();
        c->setWire(clk->id(), {0});
        c->tick();
    }
};

} // namespace

TEST(Assert, CountsFailures) {
    Fx f;
    EXPECT_EQ(f.a->failCount(), 0u);
    f.edge(false);
    EXPECT_EQ(f.a->failCount(), 1u);
    f.edge(false);
    EXPECT_EQ(f.a->failCount(), 2u);
}

TEST(Assert, PassingConditionDoesNotFail) {
    Fx f;
    f.edge(true);
    f.edge(true);
    EXPECT_EQ(f.a->failCount(), 0u);
}

TEST(Assert, NoEdgeNoFailure) {
    Fx f;
    f.c->setWire(f.cond->id(), {0});
    f.c->setWire(f.clk->id(), {0});
    for (int i = 0; i < 10; i++)
        f.c->tick();
    EXPECT_EQ(f.a->failCount(), 0u);
}

TEST(Assert, ReportLimitDoesNotAffectCount) {
    Fx f(1);
    for (int i = 0; i < 5; i++)
        f.edge(false);
    EXPECT_EQ(f.a->failCount(), 5u);
}

TEST(Assert, ZeroReportsStillCounts) {
    Fx f(0);
    f.edge(false);
    f.edge(false);
    EXPECT_EQ(f.a->failCount(), 2u);
}

TEST(Assert, MixedPassAndFail) {
    Fx f;
    f.edge(false);
    f.edge(true);
    f.edge(false);
    EXPECT_EQ(f.a->failCount(), 2u);
}
