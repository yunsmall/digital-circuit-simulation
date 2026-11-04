// Display：值变化才回调、初值触发、实例级 sink 互不干扰、pull 接口、JSON 往返
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/io/Display.h>

using namespace dsc;

namespace {

struct Event {
    const Display *owner;
    uint64_t value;
};

using Events = std::vector<Event>;

void collect(void *ctx, const Display *owner, uint64_t value) {
    static_cast<Events *>(ctx)->push_back({owner, value});
}

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Display *d = nullptr;
    Net *in;
    Events events;
    int bytes;

    Fx(int bw, Display::Kind kind = Display::Kind::Led) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<Display>("disp0", bw, kind, 1);
        d = p.get();
        d->setSink({&collect}, &events);
        c->addComponent(std::move(p));
        in = c->createNet("in");
        c->connect(d, "in", in);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    void set(uint64_t v) {
        std::vector<uint8_t> data(bytes);
        for (int i = 0; i < bytes; i++)
            data[i] = (uint8_t) (v >> (8 * i));
        c->setWire(in->id(), data);
    }
};

} // namespace

TEST(Display, FiresOnceOnFirstValue) {
    Fx f(8);
    f.set(0);
    f.c->tick();
    ASSERT_EQ(f.events.size(), 1u);
    EXPECT_EQ(f.events[0].value, 0u);
    EXPECT_EQ(f.events[0].owner, f.d);
}

TEST(Display, DoesNotFireWhenUnchanged) {
    Fx f(8);
    f.set(5);
    f.c->tick();
    f.c->tick();
    f.c->tick();
    EXPECT_EQ(f.events.size(), 1u);
}

TEST(Display, FiresOnEveryChange) {
    Fx f(8);
    f.set(1);
    f.c->tick();
    f.set(2);
    f.c->tick();
    f.set(2);
    f.c->tick();
    f.set(3);
    f.c->tick();
    ASSERT_EQ(f.events.size(), 3u);
    EXPECT_EQ(f.events[0].value, 1u);
    EXPECT_EQ(f.events[1].value, 2u);
    EXPECT_EQ(f.events[2].value, 3u);
}

TEST(Display, PullValueWorksWithoutSink) {
    Fx f(16);
    f.d->clearSink();
    f.set(0xBEEF);
    f.c->tick();
    EXPECT_EQ(f.d->value(), 0xBEEFu);
    EXPECT_EQ(f.d->kind(), Display::Kind::Led);
}

TEST(Display, OneBitWidth) {
    Fx f(1);
    f.set(1);
    f.c->tick();
    f.set(0);
    f.c->tick();
    f.set(1);
    f.c->tick();
    ASSERT_EQ(f.events.size(), 3u);
    EXPECT_EQ(f.events[2].value, 1u);
}

TEST(Display, SixtyFourBitValue) {
    Fx f(64);
    f.set(0x0123456789ABCDEFull);
    f.c->tick();
    ASSERT_EQ(f.events.size(), 1u);
    EXPECT_EQ(f.events[0].value, 0x0123456789ABCDEFull);
    EXPECT_EQ(f.d->value(), 0x0123456789ABCDEFull);
}

TEST(Display, KindAndDigitsAreKept) {
    auto p = std::make_unique<Display>("d", 8, Display::Kind::SevenSeg, 4);
    EXPECT_EQ(p->kind(), Display::Kind::SevenSeg);
    EXPECT_EQ(p->digits(), 4);
    EXPECT_EQ(p->name(), "d");
}

// 实例级绑定：两个显示件挂不同的 ctx，各自只收到自己的变化。
// 原来进程级单 sink 的写法在这里会串台
TEST(Display, InstancesRouteToTheirOwnSinks) {
    auto c = std::make_unique<Circuit>();
    Events ev1, ev2;
    auto p1 = std::make_unique<Display>("d1", 8, Display::Kind::Led, 1);
    auto p2 = std::make_unique<Display>("d2", 8, Display::Kind::SevenSeg, 2);
    auto *d1 = p1.get();
    auto *d2 = p2.get();
    d1->setSink({&collect}, &ev1);
    d2->setSink({&collect}, &ev2);
    c->addComponent(std::move(p1));
    c->addComponent(std::move(p2));
    Net *n1 = c->createNet("n1");
    Net *n2 = c->createNet("n2");
    c->connect(d1, "in", n1);
    c->connect(d2, "in", n2);
    ASSERT_EQ(c->compile().code, ErrorCode::OK);
    c->init();

    // 两个显示件第一次执行都会把初值推给各自的 sink，先把这次初值事件清掉
    c->setWire(n1->id(), 0);
    c->setWire(n2->id(), 0);
    c->tick();
    ev1.clear();
    ev2.clear();

    c->setWire(n1->id(), 0x11);
    c->tick();
    EXPECT_EQ(ev1.size(), 1u);
    EXPECT_TRUE(ev2.empty()) << "另一个实例的 sink 不应收到事件";

    c->setWire(n2->id(), 0x22);
    c->tick();
    ASSERT_EQ(ev2.size(), 1u);
    EXPECT_EQ(ev2[0].owner, d2);
    EXPECT_EQ(ev1.size(), 1u);
}

TEST(Display, RebindingReplacesPreviousSink) {
    Fx f(8);
    Events second;
    f.set(1);
    f.c->tick();
    ASSERT_EQ(f.events.size(), 1u);

    f.d->setSink({&collect}, &second);
    f.set(2);
    f.c->tick();
    EXPECT_EQ(f.events.size(), 1u) << "旧 sink 不应再收到事件";
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].value, 2u);
}

TEST(Display, ClearSinkStopsCallbacksButKeepsValue) {
    Fx f(8);
    f.set(3);
    f.c->tick();
    ASSERT_EQ(f.events.size(), 1u);

    f.d->clearSink();
    f.set(4);
    f.c->tick();
    EXPECT_EQ(f.events.size(), 1u);
    EXPECT_EQ(f.d->value(), 4u);
}

TEST(Display, ResetClearsHostValueBeforeNextTick) {
    Fx f(8);
    f.set(7);
    f.c->tick();
    ASSERT_EQ(f.d->value(), 7u);

    f.c->reset();
    EXPECT_EQ(f.d->value(), 0u) << "reset 后到下一次 tick 之间也不能读到旧值";
}

TEST(Display, JsonRoundTripKeepsParams) {
    Circuit c;
    auto *in = c.createNet("in");
    auto *d = c.addComponent(std::make_unique<Display>("d0", 16, Display::Kind::SevenSeg, 3));
    c.connect(d, "in", in);

    std::string error;
    auto c2 = Circuit::fromJson(c.exportJson(), error);
    ASSERT_NE(c2, nullptr) << error;
    ASSERT_EQ(c2->components().size(), 1u);
    auto *d2 = dynamic_cast<Display *>(c2->components()[0].get());
    ASSERT_NE(d2, nullptr);
    EXPECT_EQ(d2->name(), "d0");
    EXPECT_EQ(d2->kind(), Display::Kind::SevenSeg);
    EXPECT_EQ(d2->digits(), 3);
    EXPECT_EQ(d2->inputs()[0]->bit_width(), 16);
}
