// disconnect / removeNet 测试
#include <dcs/Circuit.h>
#include <dcs/components/combinational/BarrelShifter.h>
#include <dcs/components/combinational/Gates.h>
#include <gtest/gtest.h>
#include <set>

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

// 删掉元件后再加，新元件不能复用仍在电路里的 ID（编译前按 id 查元件会歧义）
TEST(DisconnectTest, AddAfterRemoveKeepsIdsUnique) {
    dsc::Circuit c;
    auto *a = c.addComponent(std::make_unique<dsc::UnaryGate>("a", 8, false));
    auto *b = c.addComponent(std::make_unique<dsc::UnaryGate>("b", 8, false));
    auto *d = c.addComponent(std::make_unique<dsc::UnaryGate>("d", 8, false));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(d, nullptr);

    c.removeComponent(a);
    auto *e = c.addComponent(std::make_unique<dsc::UnaryGate>("e", 8, false));
    ASSERT_NE(e, nullptr);

    std::set<int> ids;
    for (auto &comp: c.components())
        ids.insert(comp->id());
    EXPECT_EQ(ids.size(), c.components().size()) << "元件 ID 出现重复";
    EXPECT_NE(e->id(), b->id());
    EXPECT_NE(e->id(), d->id());
}

// 编译后改结构又没重新编译时，tick/setWire/getWire 必须拒绝执行过期 JIT
TEST(DisconnectTest, TickAfterStructuralChangeIsIgnored) {
    dsc::Circuit c;
    auto *n0 = c.createNet("n0"), *n1 = c.createNet("n1");
    auto *g = c.addComponent(std::make_unique<dsc::UnaryGate>("buf", 8, false));
    c.connect(g, "in", n0);
    c.connect(g, "out", n1);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.setWire(n0->id(), w(0xAA));
    c.tick();
    EXPECT_EQ(c.getWire(n1->id()), w(0xAA));

    // 结构改了但没重编译：旧 JIT 的线网编号已经对不上
    c.disconnect(g, "out");
    c.setWire(n0->id(), w(0x55));
    c.tick();
    EXPECT_EQ(c.getWire(n1->id()), w(0)) << "过期 JIT 仍在执行";
}

// 总线冲突留在 _tick_error 里，重新编译/初始化后必须清掉，否则新电路一 tick 就停摆
TEST(DisconnectTest, TickErrorClearedOnRecompile) {
    dsc::Circuit c;
    auto *n_in = c.createNet("in"), *n_bus = c.createNet("bus"), *n_en = c.createNet("en");
    auto *ts1 = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts1", 8));
    auto *ts2 = c.addComponent(std::make_unique<dsc::GateTSBUF>("ts2", 8));
    c.connect(ts1, "in", n_in);
    c.connect(ts1, "out", n_bus);
    c.connect(ts1, "oe", n_en);
    c.connect(ts2, "in", n_in);
    c.connect(ts2, "out", n_bus);
    c.connect(ts2, "oe", n_en);
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.setWire(n_in->id(), w(0x5A));
    c.setWire(n_en->id(), w(1));
    c.tick();
    ASSERT_NE(c.tickError().code, dsc::ErrorCode::OK) << "两个三态门同时使能应报冲突";

    // 拆掉一个驱动后重新编译，冲突错误必须清掉
    c.disconnect(ts2, "out");
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    c.setWire(n_in->id(), w(0x5A));
    c.setWire(n_en->id(), w(1));
    c.tick();
    EXPECT_EQ(c.getWire(n_bus->id())[0], 0x5A) << "旧的总线冲突错误把新电路卡死了";
}

// connect 对空指针有检查，disconnect/removeComponent 对空指针也不能崩
TEST(DisconnectTest, NullComponentIsIgnored) {
    dsc::Circuit c;
    c.disconnect(nullptr, "in");
    c.disconnectAll(nullptr);
    c.removeComponent(nullptr);
    SUCCEED();
}
