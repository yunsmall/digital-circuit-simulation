// tickCycle / tickCycles 是给高频仿真用的快速路径：它们在 JIT 里直接翻转时钟线网，
// 省掉外部两次跨模块调用。语义必须与"手工 setWire + tick 两次"完全一致，
// 否则所有时序电路的行为都会静默改变。
#include <dcs/Circuit.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

#include <memory>
#include <tuple>

namespace {

// 取反后接回自己的 D 触发器：每个时钟周期状态翻转一次，跑几拍就能看出差异
struct Toggle {
    std::unique_ptr<dsc::Circuit> c;
    dsc::Net *clk = nullptr;
    dsc::Net *q = nullptr;
};

Toggle build() {
    Toggle t;
    t.c = std::make_unique<dsc::Circuit>();
    t.clk = t.c->createNet("clk");
    auto *d = t.c->createNet("d");
    t.q = t.c->createNet("q");
    auto *ff = t.c->addComponent(std::make_unique<dsc::DFlipFlop>("ff", 8));
    t.c->connect(ff, "d", d);
    t.c->connect(ff, "clk", t.clk);
    t.c->connect(ff, "q", t.q);
    auto *inv = t.c->addComponent(std::make_unique<dsc::UnaryGate>("inv", 8));
    t.c->connect(inv, "in", t.q);
    t.c->connect(inv, "out", d);
    auto err = t.c->compile();
    EXPECT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
    t.c->init();
    return t;
}

void runManual(dsc::Circuit &c, dsc::Net *clk, int n) {
    for (int i = 0; i < n; i++) {
        c.setWire(clk->id(), uint64_t{0});
        c.tick();
        c.setWire(clk->id(), uint64_t{1});
        c.tick();
    }
}

} // namespace

TEST(TickCycle, MatchesManualClockToggling) {
    for (int n = 0; n <= 5; n++) {
        auto m = build();
        auto f = build();
        runManual(*m.c, m.clk, n);
        for (int i = 0; i < n; i++) f.c->tickCycle(f.clk->id());
        EXPECT_EQ(m.c->getWire(m.q->id())[0], f.c->getWire(f.q->id())[0])
            << "跑 " << n << " 个周期后 tickCycle 与手工翻转时钟的结果不一致";
    }
}

TEST(TickCycle, TickCyclesMatchesRepeatedTickCycle) {
    for (int n : {0, 1, 2, 7}) {
        auto one = build();
        auto bulk = build();
        for (int i = 0; i < n; i++) one.c->tickCycle(one.clk->id());
        bulk.c->tickCycles(bulk.clk->id(), n);
        EXPECT_EQ(one.c->getWire(one.q->id())[0], bulk.c->getWire(bulk.q->id())[0])
            << "tickCycles(" << n << ") 与逐次 tickCycle 的结果不一致";
    }
}

TEST(TickCycle, EquivalentToTwoTicksPerCycle) {
    // 时钟高电平拍与低电平拍的相位关系也要一致：手工方式跑 N 个周期后，
    // 线网上的时钟应当停在高电平
    auto t = build();
    t.c->tickCycles(t.clk->id(), 3);
    EXPECT_EQ(t.c->getWire(t.clk->id())[0], 1u) << "一个周期结束时钟应停在高电平";
}
