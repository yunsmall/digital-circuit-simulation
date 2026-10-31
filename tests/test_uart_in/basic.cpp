// UartIn：把字节编成 8N1 串行电平（UART 接收端的对端）
#include <gtest/gtest.h>

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
        auto p = std::make_unique<UartIn>("u0", baud_div);
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
    void ticks(int n) {
        for (int i = 0; i < n; i++)
            circuit->tick();
    }
    int level() const { return circuit->getWire(rx->id())[0] & 1; }
};

} // namespace

// 空闲时串行线必须是高电平，否则 UART 的接收器会一直以为有起始位
TEST(UartInTest, IdleIsHigh) {
    UartInFixture f;
    f.ticks(200);
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
        f.ticks(1);
    ASSERT_LT(guard, 50) << "没有产生起始位的下降沿";

    // 已在起始位第 1 拍，再走 3 拍到中点
    f.ticks(3);
    EXPECT_EQ(f.level(), 0) << "起始位应为低";

    int data = 0;
    for (int b = 0; b < 8; b++) {
        f.ticks(f.baud);
        data |= f.level() << b;
    }
    f.ticks(f.baud);
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
            f.ticks(1);
        ASSERT_LT(guard, 50) << "第 " << n << " 个字节没有起始沿";
        f.ticks(3);
        EXPECT_EQ(f.level(), 0);
        int data = 0;
        for (int b = 0; b < 8; b++) {
            f.ticks(f.baud);
            data |= f.level() << b;
        }
        f.ticks(f.baud);
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
