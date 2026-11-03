// UartFeed：外部 push 字节 → 8N1 串行电平
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/UartFeed.h>
#include <thread>

using namespace dsc;

namespace {

struct FeedFixture {
    std::unique_ptr<Circuit> circuit;
    UartFeed *feed = nullptr;
    Net *clk = nullptr, *rx = nullptr;
    int baud;

    explicit FeedFixture(int baud_div = 8) : baud(baud_div) {
        circuit = std::make_unique<Circuit>();
        auto p = std::make_unique<UartFeed>("f0", baud_div);
        feed = p.get();
        circuit->addComponent(std::move(p));
        clk = circuit->createNet("clk");
        rx = circuit->createNet("rx");
        circuit->connect(feed, "clk", clk);
        circuit->connect(feed, "rx", rx);
        auto err = circuit->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
    }

    void ticks(int n) {
        for (int i = 0; i < n; i++)
            circuit->tick();
    }
    int level() const { return circuit->getWire(rx->id())[0] & 1; }
};

} // namespace

TEST(UartFeedTest, IdleIsHigh) {
    FeedFixture f;
    f.ticks(200);
    EXPECT_EQ(f.level(), 1);
}

TEST(UartFeedTest, Sends8N1Frame) {
    FeedFixture f(8);
    ASSERT_TRUE(f.feed->push(0x41)); // 'A'
    EXPECT_EQ(f.feed->pending(), 1u);

    int guard = 0;
    while (f.level() == 1 && guard++ < 50)
        f.ticks(1);
    ASSERT_LT(guard, 50) << "没有产生起始位下降沿";

    f.ticks(3);
    EXPECT_EQ(f.level(), 0);
    int data = 0;
    for (int b = 0; b < 8; b++) {
        f.ticks(f.baud);
        data |= f.level() << b;
    }
    f.ticks(f.baud);
    EXPECT_EQ(data, 0x41);
    EXPECT_EQ(f.level(), 1);
    EXPECT_EQ(f.feed->pending(), 0u);
}

TEST(UartFeedTest, PushesMultipleBytesInOrder) {
    FeedFixture f(8);
    const uint8_t msg[] = {'h', 'i', '\r'};
    EXPECT_EQ(f.feed->push(msg, sizeof(msg)), sizeof(msg));
    EXPECT_EQ(f.feed->pending(), sizeof(msg));

    std::vector<int> got;
    for (size_t n = 0; n < sizeof(msg); n++) {
        int guard = 0;
        while (f.level() == 1 && guard++ < 50)
            f.ticks(1);
        ASSERT_LT(guard, 50) << "第 " << n << " 个字节没有起始沿";
        f.ticks(3);
        int data = 0;
        for (int b = 0; b < 8; b++) {
            f.ticks(f.baud);
            data |= f.level() << b;
        }
        f.ticks(f.baud);
        got.push_back(data);
    }
    EXPECT_EQ(got, std::vector<int>({'h', 'i', '\r'}));
}

// 队列满要明确返回 false，让生产者决定重试，不能覆盖还没发出去的数据
TEST(UartFeedTest, QueueFullReportsFailure) {
    FeedFixture f(8);
    size_t n = 0;
    while (f.feed->push(0x55))
        n++;
    EXPECT_EQ(n, 4095u); // 容量 4096，环形队列永远留一个空位
    EXPECT_FALSE(f.feed->push(0x55));
}

// --forever --watch 下 stdin 线程和 watch 主循环会同时 push。
// 队列本身是单生产者设计，push 必须把多生产者串行化，不能丢字节
TEST(UartFeedTest, ConcurrentProducersDoNotLoseBytes) {
    FeedFixture f(8);
    constexpr int N = 2000;
    auto producer = [&] {
        for (int i = 0; i < N; i++)
            while (!f.feed->push((uint8_t) i))
                std::this_thread::yield();
    };
    std::thread a(producer), b(producer);
    a.join();
    b.join();
    EXPECT_EQ(f.feed->pending(), 2u * N);
}
