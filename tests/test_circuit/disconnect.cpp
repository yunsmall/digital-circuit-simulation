// disconnect / removeNet 测试
#include <dcs/Circuit.h>
#include <dcs/components/BarrelShifter.h>
#include <dcs/components/Gates.h>
#include <gtest/gtest.h>

// getWire 固定返回 16 字节，辅助比较
static std::vector<uint8_t> w(uint8_t b) {
    std::vector<uint8_t> v(16, 0);
    v[0] = b;
    return v;
}

// ============================================================
// 基本 disconnect 后重连
// ============================================================
TEST(DisconnectTest, BasicReconnect) {
    dsc::Circuit c;
    auto *n0 = c.createNet("n0"), *n1 = c.createNet("n1"), *n2 = c.createNet("n2");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8, false));

    // 初始连接: n0 → buf.in, buf.out → n1
    c.connect(g, "in", n0);
    c.connect(g, "out", n1);
    c.compile();
    c.init();
    c.setWire(n0->id(), w(0xAA));
    c.tick();
    EXPECT_EQ(c.getWire(n1->id()), w(0xAA));

    // 断开输出引脚，改连到 n2
    c.disconnect(g, "out");
    c.connect(g, "out", n2);
    c.compile();
    c.init();
    c.setWire(n0->id(), w(0xAA));
    c.tick();
    EXPECT_EQ(c.getWire(n2->id()), w(0xAA));
}

// ============================================================
// disconnect 已断开的引脚应无操作
// ============================================================
TEST(DisconnectTest, AlreadyDisconnectedNoOp) {
    dsc::Circuit c;
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("not", 8));

    EXPECT_NO_THROW(c.disconnect(g, "in"));
    EXPECT_NO_THROW(c.disconnect(g, "out"));
}

// ============================================================
// removeNet 后线网被销毁，可创建同名新线网
// ============================================================
TEST(DisconnectTest, RemoveNet) {
    dsc::Circuit c;
    auto *n0 = c.createNet("n0"), *n1 = c.createNet("n1");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8, false));
    c.connect(g, "in", n0);
    c.connect(g, "out", n1);

    EXPECT_EQ(c.netCount(), 2);
    c.removeNet(n1);
    EXPECT_EQ(c.netCount(), 1);
    // 引脚已断开
    EXPECT_EQ(g->outputs()[0]->net(), nullptr);
    // 可重建同名线网
    auto *n1b = c.createNet("n1");
    c.connect(g, "out", n1b);
    c.compile();
    c.init();
    c.setWire(n0->id(), w(0x42));
    c.tick();
    EXPECT_EQ(c.getWire(n1b->id()), w(0x42));
}

// ============================================================
// disconnect 后不重连直接编译也不应崩溃
// ============================================================
TEST(DisconnectTest, CompileAfterDisconnect) {
    dsc::Circuit c;
    auto *n0 = c.createNet("n0"), *n1 = c.createNet("n1");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8, false));
    c.connect(g, "in", n0);
    c.connect(g, "out", n1);

    c.disconnect(g, "out");
    // 输出悬空时编译应正常
    EXPECT_NO_THROW(c.compile());
}

// ============================================================
// 三态门 disconnect 后 _bus_nets 清理正确
// ============================================================
TEST(DisconnectTest, TriStateBusCleanup) {
    dsc::Circuit c;
    auto *n_in = c.createNet("in"), *n_out = c.createNet("out");
    auto *ts1 = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts1", 8));
    auto *ts2 = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts2", 8));
    c.connect(ts1, "in", n_in);
    c.connect(ts1, "out", n_out);
    c.connect(ts2, "in", n_in);
    c.connect(ts2, "out", n_out);
    EXPECT_NO_THROW(c.compile());

    // 断开一个三态门，只剩一个驱动，也应编译通过
    c.disconnect(ts2, "out");
    EXPECT_NO_THROW(c.compile());
}

// ============================================================
// 断开后可重新连线
// ============================================================
TEST(DisconnectTest, ReconnectAfterDisconnect) {
    dsc::Circuit c;
    auto *n0 = c.createNet("n0"), *n1 = c.createNet("n1"), *n2 = c.createNet("n2");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8, false));

    c.connect(g, "out", n1);
    c.disconnect(g, "out");
    c.connect(g, "out", n2);
    c.connect(g, "in", n0);
    c.compile();
    c.init();
    c.setWire(n0->id(), w(0x55));
    c.tick();
    EXPECT_EQ(c.getWire(n2->id()), w(0x55));
}

// ============================================================
// disconnectAll 一次断开全部引脚
// ============================================================
TEST(DisconnectTest, DisconnectAll) {
    dsc::Circuit c;
    auto *n0 = c.createNet("n0"), *n1 = c.createNet("n1"), *n2 = c.createNet("n2");
    auto *g = c.addComponent(std::make_unique<dsc::BarrelShifter>("b", 8));
    c.connect(g, "in", n0);
    c.connect(g, "amt", n1);
    c.connect(g, "dir", n2);
    c.connect(g, "arith", n2);
    c.connect(g, "out", n1);

    c.disconnectAll(g);
    // 全部引脚应断开
    for (auto &p : g->inputs())
        EXPECT_EQ(p->net(), nullptr);
    for (auto &p : g->outputs())
        EXPECT_EQ(p->net(), nullptr);
    // 之后可重连不同线网
    auto *n3 = c.createNet("n3");
    c.connect(g, "in", n3);
    c.connect(g, "out", n3);
    EXPECT_NO_THROW(c.compile());
}

// ============================================================
// removeComponent 移除后元件数量减少
// ============================================================
TEST(DisconnectTest, RemoveComponent) {
    dsc::Circuit c;
    auto *g1 = c.addComponent(std::make_unique<dsc::UnaryGate>("g1", 8, false));
    auto *g2 = c.addComponent(std::make_unique<dsc::UnaryGate>("g2", 8, true));
    EXPECT_EQ(c.componentCount(), 2);

    c.removeComponent(g1);
    EXPECT_EQ(c.componentCount(), 1);
    // g2 仍在电路中
    EXPECT_EQ(c.components()[0].get(), g2);
    EXPECT_NO_THROW(c.compile());
}
