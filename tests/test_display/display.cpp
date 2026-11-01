// Display：值变化才回调、初值触发、多实例区分、pull 接口、注销 sink
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
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
        Display::setSink({&collect}, &events);
        auto p = std::make_unique<Display>("disp0", bw, kind, 1);
        d = p.get();
        c->addComponent(std::move(p));
        in = c->createNet("in");
        c->connect(d, "in", in);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    ~Fx() { Display::setSink({}, nullptr); }

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
    Display::setSink({}, nullptr);
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

TEST(Display, TwoInstancesHaveDistinctOwners) {
    auto c = std::make_unique<Circuit>();
    Events ev;
    Display::setSink({&collect}, &ev);
    auto p1 = std::make_unique<Display>("d1", 8, Display::Kind::Led, 1);
    auto p2 = std::make_unique<Display>("d2", 8, Display::Kind::SevenSeg, 2);
    auto *d1 = p1.get();
    auto *d2 = p2.get();
    c->addComponent(std::move(p1));
    c->addComponent(std::move(p2));
    Net *n1 = c->createNet("n1");
    Net *n2 = c->createNet("n2");
    c->connect(d1, "in", n1);
    c->connect(d2, "in", n2);
    ASSERT_EQ(c->compile().code, ErrorCode::OK);
    c->init();
    c->setWire(n1->id(), 0x11);
    c->setWire(n2->id(), 0x22);
    c->tick();
    ASSERT_GE(ev.size(), 2u);
    EXPECT_NE(ev[0].owner, ev[1].owner);
    Display::setSink({}, nullptr);
}
