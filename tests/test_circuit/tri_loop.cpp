// 单驱动三态线网绕成的反馈环。
//
// check() 不为三态输出建边（使能是运行期量，静态判断不了），所以这类环
// 只能在运行期由阶段 2b 迭代到不动点兜底。回归点：阶段 2b 曾经只把
// _bus_nets（多驱动共线）当总线，单驱动三态线网既不被判环、也不进 2b 的
// 重跑集合，环会静默地按拓扑排序的残余顺序执行一次。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>

using namespace dsc;

namespace {

// buf 的三态输出驱动 N，inv 读 N 后取反送回 buf 的输入。使能常开时
// N = NOT(N)，任何值都会在每轮补跑里翻转
struct TriFeedback {
    Circuit c;
    Net *n = nullptr, *nx = nullptr, *en = nullptr;
    Component *buf = nullptr;
    Component *inv = nullptr;

    TriFeedback() {
        n = c.createNet("n");
        nx = c.createNet("nx");
        en = c.createNet("en");
        buf = c.addComponent(std::make_unique<GateTSBUF>("buf", 8));
        inv = c.addComponent(std::make_unique<UnaryGate>("inv", 8, true));
        c.connect(buf, "in", nx);
        c.connect(buf, "out", n);
        c.connect(buf, "oe", en);
        c.connect(inv, "in", n);
        c.connect(inv, "out", nx);
    }
};

} // namespace

TEST(TriLoop, SingleDriverTriStateLoopReportsOscillation) {
    TriFeedback f;
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    f.c.setWire(f.en->id(), {1, 0});
    f.c.tick();
    ASSERT_TRUE(f.c.hasError()) << "常使能的单驱动三态反馈环必须被阶段 2b 报出来";
    EXPECT_EQ(f.c.tickError().code, ErrorCode::BUS_OSCILLATION);
}

TEST(TriLoop, SingleDriverTriStateLoopQuietWhenDisabled) {
    TriFeedback f;
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    f.c.setWire(f.en->id(), {0, 0});
    f.c.tick();
    EXPECT_FALSE(f.c.hasError()) << "三态门关闭时环是断开的，不能误报振荡";
    EXPECT_EQ(f.c.getWire(f.n->id())[0], 0);

    // 再跑几拍确认线网保持 0、错误不会延迟出现
    for (int i = 0; i < 3; i++)
        f.c.tick();
    EXPECT_FALSE(f.c.hasError());
    EXPECT_EQ(f.c.getWire(f.n->id())[0], 0);
}

// 环在中间某一拍才被打开：打开的那一拍必须立刻报振荡，而不是等下一拍
TEST(TriLoop, EnablingLoopIsDetectedInTheSameTick) {
    TriFeedback f;
    auto e = f.c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    f.c.init();
    f.c.setWire(f.en->id(), {0, 0});
    f.c.tick();
    ASSERT_FALSE(f.c.hasError());
    f.c.setWire(f.en->id(), {1, 0});
    f.c.tick();
    ASSERT_TRUE(f.c.hasError());
    EXPECT_EQ(f.c.tickError().code, ErrorCode::BUS_OSCILLATION);
}

// 直接 pin->setNet() 绕过 connect() 时，多三态驱动也必须做总线决议。
// 不重建 _bus_nets 的话生成代码会各写各的，最后写的赢且不报冲突
TEST(TriLoop, DirectSetNetTriDriversStillResolveBus) {
    Circuit c;
    Net *n = c.createNet("n");
    Net *a = c.createNet("a"), *b = c.createNet("b"), *oe = c.createNet("oe");
    auto *t1 = c.addComponent(std::make_unique<GateTSBUF>("t1", 8));
    auto *t2 = c.addComponent(std::make_unique<GateTSBUF>("t2", 8));
    c.connect(t1, "in", a);
    c.connect(t1, "oe", oe);
    c.connect(t2, "in", b);
    c.connect(t2, "oe", oe);
    // 绕过 connect：_bus_nets 不会登记这根线
    t1->outputs()[0]->setNet(n);
    t2->outputs()[0]->setNet(n);

    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    c.init();
    c.setWire(a->id(), {0x11, 0});
    c.setWire(b->id(), {0x22, 0});
    c.setWire(oe->id(), {1, 0});
    c.tick();
    ASSERT_TRUE(c.hasError()) << "两个三态输出同时驱动不同值必须报总线冲突";
    EXPECT_EQ(c.tickError().code, ErrorCode::BUS_CONFLICT);
}
