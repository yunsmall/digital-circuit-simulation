// UartIn：把字节编成 8N1 串行电平（UART 接收端的对端）
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/io/UartIn.h>

using namespace dsc;

namespace {

struct UartInFixture {
    std::unique_ptr<Circuit> circuit;
    UartIn *in = nullptr;
    Net *clk = nullptr, *rx = nullptr;
    int baud;

    explicit UartInFixture(int baud_div = 8) : baud(baud_div) {
        circuit = std::make_unique<Circuit>();
        auto p = std::make_unique<UartIn>("u0", 1, 1000000000LL / baud_div);
        in = p.get();
        circuit->addComponent(std::move(p));
        clk = circuit->createNet("clk");
        rx = circuit->createNet("rx");
        circuit->connect(in, "clk", clk);
        circuit->connect(in, "rx", rx);
        auto err = circuit->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
    }

    // seq 元件每 tick 执行一次，baud_div 的单位就是 tick
    void cycles(int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), 0);
            circuit->tick();
            circuit->setWire(clk->id(), 1);
            circuit->tick();
        }
    }
    int level() const { return circuit->getWire(rx->id())[0] & 1; }
};

} // namespace

// 空闲时串行线必须是高电平，否则 UART 的接收器会一直以为有起始位
TEST(UartInTest, IdleIsHigh) {
    UartInFixture f;
    f.cycles(200);
    EXPECT_EQ(f.level(), 1);
}

// 一个字节的完整波形：起始位 0、8 位数据 LSB 先、停止位 1
TEST(UartInTest, Sends8N1Frame) {
    UartInFixture f(8);
    f.in->pushByte(0x41); // 'A' = 0b01000001
    EXPECT_EQ(f.in->pending(), 1u);

    // 找到起始位的下降沿
    int guard = 0;
    while (f.level() == 1 && guard++ < 50)
        f.cycles(1);
    ASSERT_LT(guard, 50) << "没有产生起始位的下降沿";

    // 已在起始位第 1 拍，再走 3 拍到中点
    f.cycles(3);
    EXPECT_EQ(f.level(), 0) << "起始位应为低";

    int data = 0;
    for (int b = 0; b < 8; b++) {
        f.cycles(f.baud);
        data |= f.level() << b;
    }
    f.cycles(f.baud);
    EXPECT_EQ(data, 0x41) << "数据位顺序必须是 LSB 先";
    EXPECT_EQ(f.level(), 1) << "停止位应为高";
    EXPECT_EQ(f.in->pending(), 0u);
}

// 连续两个字节之间要靠停止位→起始位的下降沿分隔
TEST(UartInTest, SendsTwoBytesBackToBack) {
    UartInFixture f(8);
    f.in->pushByte(0x55);
    f.in->pushByte(0xAA);

    std::vector<int> bytes;
    for (int n = 0; n < 2; n++) {
        int guard = 0;
        while (f.level() == 1 && guard++ < 50)
            f.cycles(1);
        ASSERT_LT(guard, 50) << "第 " << n << " 个字节没有起始沿";
        f.cycles(3);
        EXPECT_EQ(f.level(), 0);
        int data = 0;
        for (int b = 0; b < 8; b++) {
            f.cycles(f.baud);
            data |= f.level() << b;
        }
        f.cycles(f.baud);
        EXPECT_EQ(f.level(), 1) << "停止位应为高";
        bytes.push_back(data);
    }
    EXPECT_EQ(bytes[0], 0x55);
    EXPECT_EQ(bytes[1], 0xAA);
}

// 队列满时丢新字节，而不是覆盖还没发出去的数据
TEST(UartInTest, QueueOverflowDropsNewBytes) {
    UartInFixture f(8);
    for (int i = 0; i < 100; i++)
        f.in->pushByte((uint8_t) i);
    EXPECT_EQ(f.in->pending(), 63u);
}

namespace {

// 轮询源：按顺序吐出预先放好的字节，吐完返回 -1
struct ByteSource {
    std::vector<int> bytes;
    int calls = 0;
    const UartIn *expected_owner = nullptr;
    bool owner_ok = true;

    static int poll(void *ctx, UartIn *owner) {
        auto &s = *static_cast<ByteSource *>(ctx);
        s.calls++;
        if (s.expected_owner && owner != s.expected_owner)
            s.owner_ok = false;
        if (s.bytes.empty())
            return -1;
        int b = s.bytes.front();
        s.bytes.erase(s.bytes.begin());
        return b;
    }
};

// 当前已经处在起始位第 1 拍：再走半位到中点，然后逐位采样
int decodeFromStartBit(UartInFixture &f) {
    f.cycles(3);
    EXPECT_EQ(f.level(), 0) << "起始位中点应为低";
    int data = 0;
    for (int b = 0; b < 8; b++) {
        f.cycles(f.baud);
        data |= f.level() << b;
    }
    f.cycles(f.baud);
    EXPECT_EQ(f.level(), 1) << "停止位应为高";
    return data;
}

} // namespace

// 未绑定 source 时走默认 stdin；绑定后只在轮询拍调用它，字节照常编成 8N1
TEST(UartInTest, CustomSourceFeedsBytes) {
    UartInFixture f(8);
    ByteSource src;
    src.bytes = {0x41}; // 'A'
    src.expected_owner = f.in;
    f.in->setSource({&ByteSource::poll}, &src);

    // POLL_INTERVAL 是 1024 拍，第一次轮询在第 1024 拍发生
    f.cycles(1024);
    ASSERT_EQ(src.calls, 1);
    EXPECT_TRUE(src.owner_ok) << "source 必须收到触发它的那个实例";
    EXPECT_EQ(f.level(), 0) << "取到字节后应当立刻发出起始位";
    EXPECT_EQ(decodeFromStartBit(f), 0x41);
    EXPECT_TRUE(src.bytes.empty());
}

// 队列里有数据时优先出队，不必等轮询周期，也不应该去打扰 source
TEST(UartInTest, QueueTakesPriorityOverSource) {
    UartInFixture f(8);
    ByteSource src;
    src.bytes = {0x41};
    f.in->setSource({&ByteSource::poll}, &src);
    f.in->pushByte(0x42); // 'B'

    f.cycles(1); // 队列非空，第一拍就取
    EXPECT_EQ(src.calls, 0);
    EXPECT_EQ(f.level(), 0);
    EXPECT_EQ(decodeFromStartBit(f), 0x42);

    // 帧发完之后要等轮询周期才轮到 source；轮询计数在忙时暂停，
    // 所以这里按"最多再等 2 个周期"推进，不假设精确拍数
    int guard = 0;
    while (src.calls == 0 && guard++ < 2048)
        f.cycles(1);
    ASSERT_EQ(src.calls, 1);
    EXPECT_EQ(f.level(), 0);
    EXPECT_EQ(decodeFromStartBit(f), 0x41);
}

// 两个实例各自的 source 互不干扰
TEST(UartInTest, EachInstanceUsesItsOwnSource) {
    UartInFixture a(8), b(8);
    ByteSource sa, sb;
    sa.bytes = {0x11};
    sb.bytes = {0x22};
    a.in->setSource({&ByteSource::poll}, &sa);
    b.in->setSource({&ByteSource::poll}, &sb);

    a.cycles(1024);
    b.cycles(1024);
    ASSERT_EQ(sa.calls, 1);
    ASSERT_EQ(sb.calls, 1);
    EXPECT_EQ(decodeFromStartBit(a), 0x11);
    EXPECT_EQ(decodeFromStartBit(b), 0x22);
}

// 复位只清发送状态机；宿主输入队列属于外部世界，已经推入的字节不能被吞掉
TEST(UartInTest, ResetKeepsHostInputQueue) {
    UartInFixture f(8);
    f.in->pushByte(0x41);
    f.in->pushByte(0x42);
    EXPECT_EQ(f.in->pending(), 2u);

    f.circuit->reset();
    EXPECT_EQ(f.in->pending(), 2u);
}

// 元件名来自 JSON，可以带换行；生成 C 的注释不能把名字拼进去，否则会逃逸成代码
TEST(UartInTest, NewlineInNameStillCompiles) {
    Circuit c;
    auto *clk = c.createNet("clk"), *rx = c.createNet("rx");
    auto *u = c.addComponent(std::make_unique<UartIn>("u\n0", 1, 125000000));
    ASSERT_NE(u, nullptr);
    c.connect(u, "clk", clk);
    c.connect(u, "rx", rx);
    auto err = c.compile();
    EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
}

TEST(UartInTest, JsonRoundTripKeepsTimingParams) {    UartInFixture f(6);
    auto json = f.circuit->exportJson();
    std::string error;
    auto c2 = Circuit::fromJson(json, error);
    ASSERT_NE(c2, nullptr) << error;
    auto j = nlohmann::json::parse(c2->exportJson());
    bool found = false;
    for (auto &comp: j["components"]) {
        if (comp["type"] == "uart_in") {
            EXPECT_EQ(comp["params"]["ns_per_period"], "1");
            EXPECT_EQ(comp["params"]["baud"], "166666666");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}
