// 多级总线级联：组合元件读总线 A、输出又驱动总线 B，B 再被下一级读取。
// 单轮补跑会漏掉第二级，这里验证迭代到不动点的行为。
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <dcs/Circuit.h>
#include <dcs/components/combinational/Gates.h>

using namespace dsc;

namespace {

// 最小三态时序元件：上升沿把 d 锁进 q，并常驱动输出（三态使能恒 1）。
// 用来验证"时序三态输出在同一拍内传播到 _w"的阶段 4
class TriSeqReg : public SequentialComponent {
public:
    explicit TriSeqReg(const std::string &name) : SequentialComponent(name, "tri_seq_reg") {
        addInput("clk", 1);
        addInput("d", 8);
        addOutput("q", 8, true, true);
    }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override { return "static uint8_t _tsr_q, _tsr_prev;"; }
    std::string genInitCode() const override { return "    _tsr_q = 0; _tsr_prev = 0;"; }
    std::string genResetCode() const override { return genInitCode(); }
    std::string genDeinitCode() const override { return ""; }
    std::string genFuncDef_seq() const override {
        int clk_nw = inputs()[0]->net() ? inputs()[0]->net()->bit_width() : 1;
        int d_nw = inputs()[1]->net() ? inputs()[1]->net()->bit_width() : 8;
        std::string s = "static void " + funcName_seq() + "() {\n";
        s += "    " + gen_read_wire(inputs()[0]->netId(), 1, clk_nw, "_clk") + "\n";
        s += "    " + gen_read_wire(inputs()[1]->netId(), 8, d_nw, "_d") + "\n";
        s += "    if (_clk && !_tsr_prev) _tsr_q = _d;\n";
        s += "    _tsr_prev = _clk;\n";
        s += "    " + genOutputWrite(0, "_tsr_q", 8) + "\n";
        s += "}\n";
        return s;
    }
    std::unique_ptr<Component> clone(const std::string &n) const override {
        return std::make_unique<TriSeqReg>(n);
    }
};

} // namespace


TEST(BusCascade, TwoLevelCascadeSettles) {
    Circuit c;
    auto *a = c.createNet("A");
    auto *b = c.createNet("B");

    // 总线 A：一个三态门常开，驱动 0x55
    auto *tsA = c.addComponent(std::make_unique<GateTSBUF>("tsA", 8));
    // X 读 A 取反，输出经 tsX 驱动总线 B
    auto *x = c.addComponent(std::make_unique<UnaryGate>("X", 8, true));
    auto *tsX = c.addComponent(std::make_unique<GateTSBUF>("tsX", 8));
    // Y 读 B 取反，输出到普通线网
    auto *y = c.addComponent(std::make_unique<UnaryGate>("Y", 8, true));

    auto *na = c.createNet("na"), *nx = c.createNet("nx"), *nout = c.createNet("nout");
    auto *en = c.createNet("en");  // 两个三态门的使能，独立线网，测试里置 1
    c.connect(tsA, "in", na);
    c.connect(tsA, "out", a);
    c.connect(tsA, "oe", en);
    c.connect(x, "in", a);
    c.connect(x, "out", nx);
    c.connect(tsX, "in", nx);
    c.connect(tsX, "out", b);
    c.connect(tsX, "oe", en);
    c.connect(y, "in", b);
    c.connect(y, "out", nout);

    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    c.setWire(na->id(), {0x55, 0});
    c.setWire(en->id(), {1, 0});
    c.tick();

    // A = 0x55，X = ~A = 0xAA，B = 0xAA，Y = ~B = 0x55
    EXPECT_EQ(c.getWire(a->id())[0], 0x55);
    EXPECT_EQ(c.getWire(b->id())[0], 0xAA) << "第二级总线必须看到第一级元件的本拍结果";
    EXPECT_EQ(c.getWire(nout->id())[0], 0x55) << "第二级组合元件必须看到第二级总线的本拍结果";
}

// 时序元件的三态输出在阶段3 才写入 _c_out/_c_oe；不补阶段4 的话，
// 下一拍阶段1 读到的是再上一拍的值，下游会固定晚一拍
TEST(BusCascade, SequentialTriStateVisibleInSameTick) {
    Circuit c;
    auto *bus = c.createNet("bus");
    auto *clk = c.createNet("clk"), *d = c.createNet("d");
    auto *out = c.createNet("out");
    auto *reg = c.addComponent(std::make_unique<TriSeqReg>("r"));
    auto *mon = c.addComponent(std::make_unique<UnaryGate>("mon", 8, false));
    c.connect(reg, "clk", clk);
    c.connect(reg, "d", d);
    c.connect(reg, "q", bus);
    c.connect(mon, "in", bus);
    c.connect(mon, "out", out);

    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    c.setWire(d->id(), {0x5A, 0});
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(c.getWire(bus->id())[0], 0x5A) << "时序三态输出必须在同一拍传播到总线";

    c.setWire(clk->id(), {0, 0});
    c.tick();
    EXPECT_EQ(c.getWire(out->id())[0], 0x5A) << "下游组合元件下一拍必须看到新总线值";
}

// 运行中发生总线冲突时，JIT 死循环必须返回，调用方才能观察到错误；
// 旧实现 for(;;) 永不返回，错误被永久吞掉
TEST(BusCascade, TickForeverReturnsOnRuntimeBusConflict) {
    auto c = std::make_unique<Circuit>();
    auto *bus = c->createNet("bus");
    auto *d1 = c->createNet("d1"), *d2 = c->createNet("d2");
    auto *oe1 = c->createNet("oe1"), *oe2 = c->createNet("oe2");
    auto *g1 = c->addComponent(std::make_unique<GateTSBUF>("g1", 8));
    auto *g2 = c->addComponent(std::make_unique<GateTSBUF>("g2", 8));
    c->connect(g1, "in", d1);
    c->connect(g1, "oe", oe1);
    c->connect(g1, "out", bus);
    c->connect(g2, "in", d2);
    c->connect(g2, "oe", oe2);
    c->connect(g2, "out", bus);

    auto e = c->compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c->init();
    c->setWire(d1->id(), {0x11, 0});
    c->setWire(d2->id(), {0x22, 0});
    c->setWire(oe1->id(), {1, 0});
    c->setWire(oe2->id(), {1, 0});

    std::atomic<bool> returned{false};
    std::thread th([&] {
        c->tickForever();
        returned.store(true);
    });
    for (int i = 0; i < 300 && !returned.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_TRUE(returned.load()) << "总线冲突后 tickForever 必须返回";
    EXPECT_TRUE(c->hasError());
    if (returned.load()) {
        th.join();
    } else {
        // 失败时不能让线程访问已析构的电路：泄漏电路，交给进程退出回收
        th.detach();
        (void) c.release();
    }
}
