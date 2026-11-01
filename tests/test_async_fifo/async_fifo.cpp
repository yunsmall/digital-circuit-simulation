// AsyncFifo：双时钟域行为、同步延迟、满/空边界、回绕、复位
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/sequential/AsyncFifo.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    AsyncFifo *f = nullptr;
    Net *wclk, *wen, *wdata, *rclk, *ren, *full, *empty, *rdata;
    int bytes;

    explicit Fx(int bw = 8, int depth = 4) : bytes((bw + 7) / 8) {
        auto p = std::make_unique<AsyncFifo>("af0", bw, depth);
        f = p.get();
        c->addComponent(std::move(p));
        wclk = c->createNet("wclk");
        wen = c->createNet("wen");
        wdata = c->createNet("wdata");
        rclk = c->createNet("rclk");
        ren = c->createNet("ren");
        full = c->createNet("full");
        empty = c->createNet("empty");
        rdata = c->createNet("rdata");
        for (auto [pin, net]: {std::pair{"wr_clk", wclk}, {"wr_en", wen}, {"wr_data", wdata},
                               {"rd_clk", rclk}, {"rd_en", ren}, {"wr_full", full},
                               {"rd_empty", empty}, {"rd_data", rdata}})
            c->connect(f, pin, net);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(wclk->id(), 0);
        c->setWire(rclk->id(), 0);
        c->setWire(wen->id(), 0);
        c->setWire(ren->id(), 0);
        setData(0);
        settle();
    }

    void setData(uint64_t v) {
        std::vector<uint8_t> d(bytes);
        for (int i = 0; i < bytes; i++)
            d[i] = (uint8_t) (v >> (8 * i));
        c->setWire(wdata->id(), d);
    }

    uint64_t data() const {
        auto v = c->getWire(rdata->id());
        uint64_t r = 0;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            r |= (uint64_t) v[i] << (8 * i);
        return r;
    }

    bool isFull() const { return c->getWire(full->id())[0] != 0; }
    bool isEmpty() const { return c->getWire(empty->id())[0] != 0; }

    void settle() {
        c->tick();
        c->tick();
    }

    // 只给写时钟一个上升沿，不改变读时钟
    void wrPulse() {
        c->setWire(wclk->id(), 0);
        c->tick();
        c->setWire(wclk->id(), 1);
        c->tick();
        c->setWire(wclk->id(), 0);
        c->tick();
    }

    void rdPulse() {
        c->setWire(rclk->id(), 0);
        c->tick();
        c->setWire(rclk->id(), 1);
        c->tick();
        c->setWire(rclk->id(), 0);
        c->tick();
    }

    void write(uint64_t v) {
        setData(v);
        c->setWire(wen->id(), 1);
        wrPulse();
        c->setWire(wen->id(), 0);
        settle();
    }

    // 只推进读写指针同步器（不读数据），用来跨过一次跨域延迟
    void syncReadSide() {
        rdPulse();
        rdPulse();
        settle(); // 满/空是时序输出，写完要再过一拍才上线网
    }

    // 读域腾出空间后，写域的满标志要两个写时钟才能看到
    void syncWriteSide() {
        wrPulse();
        wrPulse();
        settle();
    }

    // 读一个数；rd_empty 为真时返回 false，rd_data 保持不变
    bool read(uint64_t &v) {
        settle(); // 让上一拍写的满/空标志传播到线网
        if (isEmpty())
            return false;
        c->setWire(ren->id(), 1);
        rdPulse();
        c->setWire(ren->id(), 0);
        settle();
        v = data();
        return true;
    }
};

} // namespace

TEST(AsyncFifo, EmptyAtReset) {
    Fx f;
    EXPECT_TRUE(f.isEmpty());
    EXPECT_FALSE(f.isFull());
    uint64_t v = 0xFFFF;
    EXPECT_FALSE(f.read(v));
    EXPECT_TRUE(f.isEmpty());
}

// 写域写入后，读域要经过两级同步才看到数据
TEST(AsyncFifo, ReadSeesDataAfterTwoSyncEdges) {
    Fx f;
    f.write(0x11);
    f.write(0x22);
    EXPECT_TRUE(f.isEmpty()) << "同步器未推进时读域还不能看到数据";
    f.syncReadSide();
    EXPECT_FALSE(f.isEmpty());

    uint64_t v = 0;
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 0x11u);
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 0x22u);
    EXPECT_TRUE(f.isEmpty());
}

// 写满后多出来的写请求必须被丢弃
TEST(AsyncFifo, FullDropsExtraWrites) {
    Fx f(8, 4);
    for (uint64_t i = 1; i <= 4; i++)
        f.write(i);
    EXPECT_TRUE(f.isFull());
    f.write(0x55); // 满，应该丢掉

    f.syncReadSide();
    for (uint64_t i = 1; i <= 4; i++) {
        uint64_t v = 0;
        ASSERT_TRUE(f.read(v)) << "第 " << i << " 个元素";
        EXPECT_EQ(v, i);
    }
    EXPECT_TRUE(f.isEmpty());
}

// 指针回绕：写 1..4、读 1..4 后继续写 5..8，数据顺序不能乱
TEST(AsyncFifo, WrapAroundPreservesOrder) {
    Fx f(8, 4);
    for (uint64_t i = 1; i <= 4; i++)
        f.write(i);
    f.syncReadSide();
    for (uint64_t i = 1; i <= 4; i++) {
        uint64_t v = 0;
        ASSERT_TRUE(f.read(v));
        EXPECT_EQ(v, i);
    }

    // 读域已经把 4 个槽位腾空，但写域要过两个写时钟才看到满标志解除
    f.syncWriteSide();
    for (uint64_t i = 5; i <= 8; i++)
        f.write(i);
    f.syncReadSide();
    for (uint64_t i = 5; i <= 8; i++) {
        uint64_t v = 0;
        ASSERT_TRUE(f.read(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_TRUE(f.isEmpty());
}

// 两边时钟同一拍上升：读到的必须是写之前的数据
TEST(AsyncFifo, SimultaneousEdgesReadOldData) {
    Fx f;
    f.write(1);
    f.syncReadSide();
    ASSERT_FALSE(f.isEmpty());

    f.setData(2);
    f.c->setWire(f.wen->id(), 1);
    f.c->setWire(f.ren->id(), 1);
    f.c->setWire(f.wclk->id(), 0);
    f.c->setWire(f.rclk->id(), 0);
    f.c->tick();
    f.c->setWire(f.wclk->id(), 1);
    f.c->setWire(f.rclk->id(), 1);
    f.c->tick();
    f.c->setWire(f.wclk->id(), 0);
    f.c->setWire(f.rclk->id(), 0);
    f.c->tick();
    f.c->setWire(f.wen->id(), 0);
    f.c->setWire(f.ren->id(), 0);
    f.settle();

    // 同拍上升沿里读到的数据挂在 rd_data 上：必须是写入之前的值 1
    EXPECT_EQ(f.data(), 1u) << "同一拍写入的数据不能出现在这次读里";

    f.syncReadSide();
    uint64_t v = 0;
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 2u);
}

// 只有写使能、没有上升沿时不能写入
TEST(AsyncFifo, NoClockEdgeNoWrite) {
    Fx f;
    f.setData(0x33);
    f.c->setWire(f.wen->id(), 1);
    for (int i = 0; i < 5; i++)
        f.c->tick();
    f.c->setWire(f.wen->id(), 0);
    f.syncReadSide();
    EXPECT_TRUE(f.isEmpty());
}

// 位宽上限和不用的高位必须被掩掉
TEST(AsyncFifo, MasksDataAboveWidth) {
    Fx f(4, 2);
    f.write(0x1F);
    f.syncReadSide();
    uint64_t v = 0;
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 0xFu);
}

// 深度 2 的边界：只能存两个
TEST(AsyncFifo, MinimumDepth) {
    Fx f(8, 2);
    f.write(0xAA);
    f.write(0xBB);
    EXPECT_TRUE(f.isFull());
    f.write(0xCC); // 丢弃
    f.syncReadSide();
    uint64_t v = 0;
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 0xAAu);
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 0xBBu);
    EXPECT_TRUE(f.isEmpty());
}

// 位宽 64 全位宽
TEST(AsyncFifo, SixtyFourBitData) {
    Fx f(64, 2);
    f.write(0xFEDCBA9876543210ull);
    f.syncReadSide();
    uint64_t v = 0;
    ASSERT_TRUE(f.read(v));
    EXPECT_EQ(v, 0xFEDCBA9876543210ull);
}

// 复位后内容与指针都清零
TEST(AsyncFifo, ResetClearsEverything) {
    Fx f;
    f.write(0x42);
    f.syncReadSide();
    ASSERT_FALSE(f.isEmpty());

    f.c->init();
    f.c->setWire(f.wclk->id(), 0);
    f.c->setWire(f.rclk->id(), 0);
    f.settle();

    EXPECT_TRUE(f.isEmpty());
    EXPECT_FALSE(f.isFull());
    uint64_t v = 0xFFFF;
    EXPECT_FALSE(f.read(v));
}

// 构造函数参数校验
TEST(AsyncFifo, RejectsBadParameters) {
    EXPECT_THROW(AsyncFifo("x", 0, 4), std::invalid_argument);
    EXPECT_THROW(AsyncFifo("x", 65, 4), std::invalid_argument);
    EXPECT_THROW(AsyncFifo("x", 8, 1), std::invalid_argument);
    EXPECT_THROW(AsyncFifo("x", 8, 3), std::invalid_argument);
    EXPECT_THROW(AsyncFifo("x", 8, 512), std::invalid_argument);
}
