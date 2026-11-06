// tickForever 的正常退出路径：interruptTickForever() 只打断 JIT 死循环，
// 不是仿真生命周期操作，也不置运行错误
#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

TEST(TickForeverInterrupt, EndsLoopWithoutError) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *y = c.createNet("y");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 1, true));
    c.connect(g, "in", a);
    c.connect(g, "out", y);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();

    std::thread stopper([&c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        c.interruptTickForever();
    });
    c.tickForever();
    stopper.join();

    EXPECT_FALSE(c.hasError());
    c.deinit();
}

// 停止标志在 init() 里清零：停止过的电路重新 init 后，tickForever 不会立刻返回
TEST(TickForeverInterrupt, InitClearsFlag) {
    dsc::Circuit c;
    auto *a = c.createNet("a");
    auto *y = c.createNet("y");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("g", 1, true));
    c.connect(g, "in", a);
    c.connect(g, "out", y);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.interruptTickForever();
    c.init();

    const auto t0 = std::chrono::steady_clock::now();
    std::thread stopper([&c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        c.interruptTickForever();
    });
    c.tickForever();
    stopper.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 80);
    EXPECT_FALSE(c.hasError());
    c.deinit();
}
