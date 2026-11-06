// 统一边沿机制：PinEdge 声明、JIT 边沿标志、genGetWireEdge、伪时钟和 timescale。
#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <dcs/Circuit.h>
#include <dcs/Component.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/combinational/Gates.h>
#include <dcs/components/platform/CLINT.h>
#include <dcs/components/sequential/Sequential.h>

using namespace dsc;

namespace {

// 最小测试元件：只有 clk/数据输入和计数输出，用来观察边沿是否触发。
// declare_edge=false 时输入不声明边沿，用来验证伪时钟接线报错。
class EdgeCounter : public SequentialComponent {
public:
    EdgeCounter(const std::string &name, PinEdge edge, int width = 1, bool declare_edge = true) :
        SequentialComponent(name, "edge_counter"), _edge(edge) {
        addInput("clk", width, declare_edge ? edge : PinEdge::None);
        addOutput("count", 32, false, true);
    }

    std::string genStateDecl() const override {
        return std::format("static uint32_t _ec_count_{};", _id);
    }
    std::string genInitCode() const override {
        return std::format("    _ec_count_{} = 0;", _id);
    }
    std::string genFuncDef_seq() const override {
        std::string c = std::format("static void {}() {{\n", funcName_seq());
        c += "    if (!(" + genGetWireEdge(0, _edge) + ")) return;\n";
        c += std::format("    _ec_count_{}++;\n", _id);
        c += "    " + genOutputWrite(0, std::format("_ec_count_{}", _id), 32) + "\n";
        c += "}\n";
        return c;
    }
    std::unique_ptr<Component> clone(const std::string &n) const override {
        return std::make_unique<EdgeCounter>(n, _edge, inputs()[0]->bit_width(), inputs()[0]->edge() != PinEdge::None);
    }

private:
    PinEdge _edge;
};

// 模拟 CPU 的旧式读法：不声明 PinEdge，每拍直接读 _w 的 clk 电平并输出。
// 用来验证 clkgen 必须在边沿检测之前执行，否则它会比 genGetWireEdge 元件晚一拍
class LevelProbe : public SequentialComponent {
public:
    explicit LevelProbe(const std::string &name) : SequentialComponent(name, "level_probe") {
        addInput("clk", 1);
        addOutput("level", 1, false, true);
    }
    std::string genFuncDef_seq() const override {
        std::string c = std::format("static void {}() {{\n", funcName_seq());
        c += "    " + gen_read_wire(inputs()[0]->netId(), 1, 1, "_lv") + "\n";
        c += "    " + genOutputWrite(0, "_lv", 1) + "\n";
        c += "}\n";
        return c;
    }
    std::unique_ptr<Component> clone(const std::string &n) const override {
        return std::make_unique<LevelProbe>(n);
    }
};

uint32_t readCount(Circuit &c, Net *n) {
    auto v = c.getWire(n->id());
    return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
}

struct EdgeFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    EdgeCounter *a = nullptr;
    Net *sig = nullptr, *out = nullptr;

    explicit EdgeFixture(PinEdge edge, int width = 1, bool declare_edge = true) {
        auto p = std::make_unique<EdgeCounter>("ec", edge, width, declare_edge);
        a = p.get();
        c->addComponent(std::move(p));
        sig = c->createNet("sig");
        out = c->createNet("out");
        c->connect(a, "clk", sig);
        c->connect(a, "count", out);
    }
    void compile() {
        auto e = c->compile();
        ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
        c->init();
        c->setWire(sig->id(), 0);
        for (int i = 0; i < 2; ++i) tick();
    }
    void tick() {
        c->tick();
    }
    void cycle(uint8_t level) {
        c->setWire(sig->id(), 0);
        tick();
        c->setWire(sig->id(), level);
        tick();
        c->setWire(sig->id(), 0);
        tick();
    }
    uint32_t count() { return readCount(*c, out); }
};

} // namespace

TEST(Clocking, RisingEdgeOnly) {
    EdgeFixture f(PinEdge::Rising);
    f.compile();
    f.cycle(1);
    EXPECT_EQ(f.count(), 1u);
    // 保持高电平不会产生新的上升沿
    for (int i = 0; i < 5; ++i)
        f.tick();
    EXPECT_EQ(f.count(), 1u) << "没有新的 0→1 不能重复触发";
    f.cycle(1);
    EXPECT_EQ(f.count(), 2u);
}

TEST(Clocking, FallingEdgeOnly) {
    EdgeFixture f(PinEdge::Falling);
    f.compile();
    // 先制造 0→1，下降沿元件不应触发
    f.c->setWire(f.sig->id(), 1);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 0u);
    f.c->setWire(f.sig->id(), 0);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u);
}

TEST(Clocking, ChangeOnMultiBit) {
    EdgeFixture f(PinEdge::Change, 8);
    f.compile();
    f.c->setWire(f.sig->id(), 0x11);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u);
    // 相同值不算变化
    f.tick();
    EXPECT_EQ(f.count(), 1u);
    f.c->setWire(f.sig->id(), 0x12);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 2u);
}

TEST(Clocking, SharedClockUpdatesAllComponents) {
    Circuit c;
    auto p1 = std::make_unique<EdgeCounter>("a", PinEdge::Rising);
    auto p2 = std::make_unique<EdgeCounter>("b", PinEdge::Rising);
    auto *ca = p1.get();
    auto *cb = p2.get();
    c.addComponent(std::move(p1));
    c.addComponent(std::move(p2));
    Net *sig = c.createNet("sig");
    Net *oa = c.createNet("oa");
    Net *ob = c.createNet("ob");
    c.connect(ca, "clk", sig);
    c.connect(cb, "clk", sig);
    c.connect(ca, "count", oa);
    c.connect(cb, "count", ob);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();
    c.setWire(sig->id(), 0);
    for (int i = 0; i < 3; ++i) {
        c.setWire(sig->id(), 0); c.tick();
        c.setWire(sig->id(), 1); c.tick();
        c.setWire(sig->id(), 0); c.tick();
    }
    EXPECT_EQ(readCount(c, oa), 3u);
    EXPECT_EQ(readCount(c, ob), 3u);
}

TEST(Clocking, PseudoClockRisesEveryTick) {
    Circuit c;
    auto g = std::make_unique<ClockGen>("g", 1, 1, PinEdge::Rising);
    auto *gen = g.get();
    auto p = std::make_unique<EdgeCounter>("ec", PinEdge::Rising);
    auto *counter = p.get();
    c.addComponent(std::move(g));
    c.addComponent(std::move(p));
    Net *sig = c.createNet("sig");
    Net *out = c.createNet("out");
    c.connect(gen, "clk", sig);
    c.connect(counter, "clk", sig);
    c.connect(counter, "count", out);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();
    for (int i = 0; i < 5; ++i)
        c.tick();
    EXPECT_EQ(readCount(c, out), 5u) << "伪时钟每 tick 都应触发";
    EXPECT_EQ(c.getWire(sig->id())[0], 0) << "伪时钟没有电平语义，恒为 0";
}

TEST(Clocking, PseudoClockRejectsOppositeEdge) {
    Circuit c;
    auto g = std::make_unique<ClockGen>("g", 1, 1, PinEdge::Rising);
    auto *gen = g.get();
    c.addComponent(std::move(g));
    auto p = std::make_unique<EdgeCounter>("ec", PinEdge::Falling);
    auto *counter = p.get();
    c.addComponent(std::move(p));
    Net *sig = c.createNet("sig");
    c.connect(gen, "clk", sig);
    c.connect(counter, "clk", sig);
    auto e = c.compile();
    EXPECT_NE(e.code, ErrorCode::OK) << "上升伪时钟不能给下降沿元件用";
    EXPECT_NE(e.message.find("伪时钟"), std::string::npos) << e.message;
}

TEST(Clocking, PseudoClockRejectsUndeclaredInput) {
    Circuit c;
    auto g = std::make_unique<ClockGen>("g", 1, 1, PinEdge::Rising);
    auto *gen = g.get();
    c.addComponent(std::move(g));
    auto p = std::make_unique<EdgeCounter>("ec", PinEdge::None, 1, false);
    auto *sink = p.get();
    c.addComponent(std::move(p));
    Net *sig = c.createNet("sig");
    c.connect(gen, "clk", sig);
    c.connect(sink, "clk", sig);
    auto e = c.compile();
    EXPECT_NE(e.code, ErrorCode::OK) << "没声明边沿的引脚不能接伪时钟";
    EXPECT_NE(e.message.find("伪时钟"), std::string::npos) << e.message;
}

TEST(Clocking, ChangeDifferentWidthsShareNet) {
    // 同一根总线上 1 位和 16 位 Change 引脚各看各的宽度：
    // 只有高字节变化时，1 位引脚不能触发
    Circuit c;
    auto p1 = std::make_unique<EdgeCounter>("a", PinEdge::Change, 1);
    auto p2 = std::make_unique<EdgeCounter>("b", PinEdge::Change, 16);
    auto *ca = p1.get();
    auto *cb = p2.get();
    c.addComponent(std::move(p1));
    c.addComponent(std::move(p2));
    Net *sig = c.createNet("sig");
    Net *oa = c.createNet("oa");
    Net *ob = c.createNet("ob");
    c.connect(ca, "clk", sig);
    c.connect(cb, "clk", sig);
    c.connect(ca, "count", oa);
    c.connect(cb, "count", ob);

    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    auto step = [&](uint8_t lo, uint8_t hi) {
        c.setWire(sig->id(), {lo, hi});
        c.tick();
        c.tick();
    };
    step(0x01, 0x00);
    EXPECT_EQ(readCount(c, oa), 1u);
    EXPECT_EQ(readCount(c, ob), 1u);

    // 只有第二字节变化：16 位引脚触发，1 位引脚看不到
    step(0x01, 0x01);
    EXPECT_EQ(readCount(c, oa), 1u) << "1 位 Change 只应看 bit0";
    EXPECT_EQ(readCount(c, ob), 2u);

    // bit0 也变化：两个引脚都触发
    step(0x00, 0x01);
    EXPECT_EQ(readCount(c, oa), 2u);
    EXPECT_EQ(readCount(c, ob), 3u);
}

TEST(Clocking, ChangeSameWidthSharesSnapshot) {
    // 同一线网、同一位宽的多个 Change 引脚共用一份快照，一次变化都要触发
    Circuit c;
    auto p1 = std::make_unique<EdgeCounter>("a", PinEdge::Change, 8);
    auto p2 = std::make_unique<EdgeCounter>("b", PinEdge::Change, 8);
    auto *ca = p1.get();
    auto *cb = p2.get();
    c.addComponent(std::move(p1));
    c.addComponent(std::move(p2));
    Net *sig = c.createNet("sig");
    Net *oa = c.createNet("oa");
    Net *ob = c.createNet("ob");
    c.connect(ca, "clk", sig);
    c.connect(cb, "clk", sig);
    c.connect(ca, "count", oa);
    c.connect(cb, "count", ob);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    auto step = [&](uint8_t v) {
        c.setWire(sig->id(), {v});
        c.tick();
        c.tick();
    };
    step(0x12);
    EXPECT_EQ(readCount(c, oa), 1u);
    EXPECT_EQ(readCount(c, ob), 1u);
    step(0x13);
    EXPECT_EQ(readCount(c, oa), 2u);
    EXPECT_EQ(readCount(c, ob), 2u);
}

TEST(Clocking, ChangeAndRisingShareNet) {
    // 1 位 Change 引脚也建立沿标志：和 Rising 引脚接同一根线时两者都要工作
    Circuit c;
    auto p1 = std::make_unique<EdgeCounter>("chg", PinEdge::Change, 1);
    auto p2 = std::make_unique<EdgeCounter>("rise", PinEdge::Rising, 1);
    auto *cc = p1.get();
    auto *cr = p2.get();
    c.addComponent(std::move(p1));
    c.addComponent(std::move(p2));
    Net *sig = c.createNet("sig");
    Net *oc = c.createNet("oc");
    Net *or_ = c.createNet("or");
    c.connect(cc, "clk", sig);
    c.connect(cr, "clk", sig);
    c.connect(cc, "count", oc);
    c.connect(cr, "count", or_);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    auto level = [&](uint8_t v) {
        c.setWire(sig->id(), {v});
        c.tick();
        c.tick();
    };
    level(1);
    EXPECT_EQ(readCount(c, oc), 1u);
    EXPECT_EQ(readCount(c, or_), 1u);
    level(0);
    EXPECT_EQ(readCount(c, oc), 2u) << "下降也是 Change";
    EXPECT_EQ(readCount(c, or_), 1u) << "下降不是上升沿";
    level(1);
    EXPECT_EQ(readCount(c, oc), 3u);
    EXPECT_EQ(readCount(c, or_), 2u);
}

TEST(Clocking, ChangeThreeAndSixteenBits) {
    // 3 位（不足一字节）和 16 位（跨字节）两组各自独立判断
    Circuit c;
    auto p1 = std::make_unique<EdgeCounter>("a", PinEdge::Change, 3);
    auto p2 = std::make_unique<EdgeCounter>("b", PinEdge::Change, 16);
    auto *ca = p1.get();
    auto *cb = p2.get();
    c.addComponent(std::move(p1));
    c.addComponent(std::move(p2));
    Net *sig = c.createNet("sig");
    Net *oa = c.createNet("oa");
    Net *ob = c.createNet("ob");
    c.connect(ca, "clk", sig);
    c.connect(cb, "clk", sig);
    c.connect(ca, "count", oa);
    c.connect(cb, "count", ob);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    auto step = [&](uint8_t lo, uint8_t hi) {
        c.setWire(sig->id(), {lo, hi});
        c.tick();
        c.tick();
    };
    step(0x01, 0x00);
    EXPECT_EQ(readCount(c, oa), 1u);
    EXPECT_EQ(readCount(c, ob), 1u);
    // bit2 在 3 位引脚范围内，两边都触发
    step(0x05, 0x00);
    EXPECT_EQ(readCount(c, oa), 2u);
    EXPECT_EQ(readCount(c, ob), 2u);
    // 只有高字节变化：3 位引脚不动
    step(0x05, 0x01);
    EXPECT_EQ(readCount(c, oa), 2u);
    EXPECT_EQ(readCount(c, ob), 3u);
    // 低 3 位变化，两边都触发
    step(0x04, 0x01);
    EXPECT_EQ(readCount(c, oa), 3u);
    EXPECT_EQ(readCount(c, ob), 4u);
}

TEST(Clocking, ChangeUnconnectedPinNeverFires) {
    Circuit c;
    auto p = std::make_unique<EdgeCounter>("a", PinEdge::Change, 8);
    auto *ca = p.get();
    c.addComponent(std::move(p));
    Net *out = c.createNet("out");
    c.connect(ca, "count", out);
    // clk 悬空：没有线网可检测，genGetWireEdge 返回常量 false
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();
    for (int i = 0; i < 3; i++)
        c.tick();
    EXPECT_EQ(readCount(c, out), 0u);
}

TEST(Clocking, ChangeCountsAgainAfterReset) {
    EdgeFixture f(PinEdge::Change, 8);
    f.compile();
    f.c->setWire(f.sig->id(), 0x21);
    f.tick();
    f.tick();
    ASSERT_EQ(f.count(), 1u);
    f.c->reset();
    f.c->setWire(f.sig->id(), 0x21);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u) << "复位清空快照后，同样的变化应重新触发";
}

TEST(Clocking, PseudoClockRejectsChangePin) {
    Circuit c;
    auto g = std::make_unique<ClockGen>("g", 1, 1, PinEdge::Rising);
    auto *gen = g.get();
    c.addComponent(std::move(g));
    auto p = std::make_unique<EdgeCounter>("a", PinEdge::Change, 8);
    auto *ca = p.get();
    c.addComponent(std::move(p));
    Net *sig = c.createNet("sig");
    c.connect(gen, "clk", sig);
    c.connect(ca, "clk", sig);
    auto e = c.compile();
    EXPECT_NE(e.code, ErrorCode::OK) << "伪时钟只有一个 1 位沿，不能接多位 Change 引脚";
    EXPECT_NE(e.message.find("伪时钟"), std::string::npos) << e.message;
}

TEST(Clocking, ChangeIgnoresUnusedHighBits) {
    // 12 位线网的槽里可能有超出有效位的高位垃圾，Change 只能看低 12 位
    EdgeFixture f(PinEdge::Change, 12);
    f.compile();
    f.c->setWire(f.sig->id(), {0x12, 0x0F});
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u);
    // 第二字节 0x0F→0x1F：高 4 位（无效位）变了，低 4 位不变
    f.c->setWire(f.sig->id(), {0x12, 0x1F});
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u) << "有效 12 位没变，不能因槽里的高位垃圾触发";
    // 第二字节低 4 位变化才是有效变化
    f.c->setWire(f.sig->id(), {0x12, 0x1E});
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 2u);
}

TEST(Clocking, NegativeBaudRejected) {
    const char *json = R"({
      "$meta": { "kind": "circuit", "format": 1 },
      "nets": ["clk"],
      "components": [
        { "type": "uart_feed", "name": "u",
          "params": { "baud": "-1" },
          "connections": { "clk": "clk" } }
      ]
    })";
    std::string error;
    auto c = Circuit::fromJson(json, error, "", nullptr);
    EXPECT_EQ(c, nullptr) << "负波特率不能静默变成极快波形";
    EXPECT_NE(error.find("baud"), std::string::npos) << error;
}

TEST(Clocking, ClintAdvancesMtimeOnRisingEdge) {
    // CLINT 的 mtime 必须在每个 clk 上升沿 +1。整机启动卡在早期启动代码时，
    // 先排除"CLINT 统一边沿标志没生效、mtime 不涨"这个可能
    Circuit c;
    Net *clk = c.createNet("clk");
    Net *adr = c.createNet("adr");
    Net *wdat = c.createNet("wdat");
    Net *we = c.createNet("we");
    Net *sel = c.createNet("sel");
    Net *stb = c.createNet("stb");
    Net *cyc = c.createNet("cyc");
    Net *dat = c.createNet("dat");
    Net *ack = c.createNet("ack");
    Net *mti = c.createNet("mti");
    Net *msi = c.createNet("msi");

    auto make_const = [&](const char *name, const char *key, const char *val) {
        std::string err;
        auto comp = ComponentFactory::instance().create(
                "constant", name, {{"bit_width", key}, {"value", val}}, err);
        EXPECT_TRUE(comp) << err;
        return c.addComponent(std::move(comp));
    };

    auto *g = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::None));
    auto *cl = c.addComponent(std::make_unique<CLINT>("cl"));
    auto *adr_c = make_const("adr_c", "32", "49144"); // 0xBFF8: mtime 低 32 位
    auto *wdat_c = make_const("wdat_c", "32", "0");
    auto *we_c = make_const("we_c", "1", "0");
    auto *sel_c = make_const("sel_c", "4", "15");
    auto *stb_c = make_const("stb_c", "1", "1");
    auto *cyc_c = make_const("cyc_c", "1", "1");

    c.connect(g, "clk", clk);
    c.connect(cl, "clk", clk);
    c.connect(cl, "wb_adr_i", adr);
    c.connect(cl, "wb_dat_i", wdat);
    c.connect(cl, "wb_we_i", we);
    c.connect(cl, "wb_sel_i", sel);
    c.connect(cl, "wb_stb_i", stb);
    c.connect(cl, "wb_cyc_i", cyc);
    c.connect(cl, "wb_dat_o", dat);
    c.connect(cl, "wb_ack_o", ack);
    c.connect(cl, "mti", mti);
    c.connect(cl, "msi", msi);
    c.connect(adr_c, "out", adr);
    c.connect(wdat_c, "out", wdat);
    c.connect(we_c, "out", we);
    c.connect(sel_c, "out", sel);
    c.connect(stb_c, "out", stb);
    c.connect(cyc_c, "out", cyc);

    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    auto mtime = [&] {
        auto v = c.getWire(dat->id());
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    };
    // clkgen 1/1：tick2/4/6/8/10/12 是上升沿。输出要下一拍才进 _w
    for (int i = 0; i < 12; i++)
        c.tick();
    EXPECT_EQ(mtime(), 6u) << "12 拍应有 6 个上升沿";
    for (int i = 0; i < 2; i++)
        c.tick();
    EXPECT_EQ(mtime(), 7u);
}

TEST(Clocking, ChangeTwoOneBitPinsShareSlot) {
    // 两个 1 位 Change 引脚共用同一个槽位：一次变化两个都触发
    Circuit c;
    auto p1 = std::make_unique<EdgeCounter>("a", PinEdge::Change, 1);
    auto p2 = std::make_unique<EdgeCounter>("b", PinEdge::Change, 1);
    auto *ca = p1.get();
    auto *cb = p2.get();
    c.addComponent(std::move(p1));
    c.addComponent(std::move(p2));
    Net *sig = c.createNet("sig");
    Net *oa = c.createNet("oa");
    Net *ob = c.createNet("ob");
    c.connect(ca, "clk", sig);
    c.connect(cb, "clk", sig);
    c.connect(ca, "count", oa);
    c.connect(cb, "count", ob);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    auto step = [&](uint8_t v) {
        c.setWire(sig->id(), {v});
        c.tick();
        c.tick();
    };
    step(1);
    EXPECT_EQ(readCount(c, oa), 1u);
    EXPECT_EQ(readCount(c, ob), 1u);
    step(0);
    EXPECT_EQ(readCount(c, oa), 2u);
    EXPECT_EQ(readCount(c, ob), 2u);
}

TEST(Clocking, Change127IgnoresTopBit) {
    // 127 位：第 16 字节的最高位不属于引脚，变化不能触发
    EdgeFixture f(PinEdge::Change, 127);
    f.compile();
    std::vector<uint8_t> v(16, 0);
    v[0] = 0x01;
    f.c->setWire(f.sig->id(), v);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u);

    v[15] = 0x80;
    f.c->setWire(f.sig->id(), v);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u) << "bit127 不在 127 位引脚范围内";

    v[15] = 0x40;
    f.c->setWire(f.sig->id(), v);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 2u);
}

TEST(Clocking, Change128UsesAllBytes) {
    EdgeFixture f(PinEdge::Change, 128);
    f.compile();
    std::vector<uint8_t> v(16, 0);
    v[0] = 0x01;
    f.c->setWire(f.sig->id(), v);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 1u);

    v[15] = 0x80;
    f.c->setWire(f.sig->id(), v);
    f.tick();
    f.tick();
    EXPECT_EQ(f.count(), 2u) << "128 位的第 16 字节是有效位";
}

TEST(Clocking, ClockSourceRunsBeforeEdgeDetection) {
    // clkgen 的输出直写 _w。它必须先于边沿检测和阶段 3 执行：否则"按电平读 clk"
    // 的元件（CPU）看到本拍电平，而 genGetWireEdge 元件看到上一拍的沿，差一拍
    Circuit c;
    Net *clk = c.createNet("clk");
    Net *level = c.createNet("level");
    Net *count = c.createNet("count");

    auto *g = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::None));
    auto *p = c.addComponent(std::make_unique<LevelProbe>("p"));
    auto *e = c.addComponent(std::make_unique<EdgeCounter>("e", PinEdge::Rising));

    c.connect(g, "clk", clk);
    c.connect(p, "clk", clk);
    c.connect(e, "clk", clk);
    c.connect(p, "level", level);
    c.connect(e, "count", count);

    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    c.init();

    c.tick();
    EXPECT_EQ(c.getWire(level->id())[0], 1) << "clkgen 必须在同一拍先把电平写进线网";
    EXPECT_EQ(readCount(c, count), 1u) << "边沿检测必须和电平看到同一个相位";

    c.tick();
    EXPECT_EQ(c.getWire(level->id())[0], 0);
    EXPECT_EQ(readCount(c, count), 1u) << "下降沿不触发上升沿计数";
}

TEST(Clocking, AsyncResetWorksBetweenEdges) {
    // 异步复位不能因为迁移到统一沿标志就被关进"只在沿执行"的分支
    Circuit c;
    Net *clk = c.createNet("clk");
    Net *d = c.createNet("d");
    Net *rst = c.createNet("rst");
    Net *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<DFlipFlop>("ff", 8, false, true, false));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "rst", rst);
    c.connect(ff, "q", q);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();

    c.setWire(rst->id(), {0});
    c.setWire(d->id(), {5});
    // 制造一个上升沿写入 q=5
    c.setWire(clk->id(), {0});
    c.tick();
    c.setWire(clk->id(), {1});
    c.tick();
    c.setWire(clk->id(), {0});
    c.tick();
    ASSERT_EQ(c.getWire(q->id())[0], 5u);

    // clk 保持低，不产生上升沿；rst 拉高必须立刻清零
    c.setWire(rst->id(), {1});
    c.tick();
    EXPECT_EQ(c.getWire(q->id())[0], 0u) << "异步复位在非上升沿拍也应生效";
}

TEST(Clocking, TimescaleFeedsParamExpressions) {
    const char *json = R"({
      "$meta": { "kind": "circuit", "format": 1, "timescale": { "tick_ns": 5 } },
      "nets": ["clk"],
      "components": [
        { "type": "clkgen", "name": "g",
          "params": { "high_ticks": "${ns_per_tick*2}" },
          "connections": { "clk": "clk" } }
      ]
    })";
    std::string error;
    auto c = Circuit::fromJson(json, error, "", nullptr);
    ASSERT_NE(c, nullptr) << error;
    auto out = nlohmann::json::parse(c->exportJson());
    bool found = false;
    for (auto &comp: out["components"]) {
        if (comp["name"] == "g") {
            EXPECT_EQ(comp["params"]["high_ticks"], "10");
            found = true;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_NE(c->generateC().find("#define DCS_TICK_NS 5"), std::string::npos);
}

TEST(Clocking, UnknownExpressionVariableFails) {
    const char *json = R"({
      "$meta": { "kind": "circuit", "format": 1 },
      "nets": ["clk"],
      "components": [
        { "type": "clkgen", "name": "g",
          "params": { "high_ticks": "${no_such_var}" },
          "connections": { "clk": "clk" } }
      ]
    })";
    std::string error;
    auto c = Circuit::fromJson(json, error, "", nullptr);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(error.find("no_such_var"), std::string::npos) << error;
}

// 分频器也是时钟源：下游边沿元件必须在分频输出上升沿的同一拍计数。
// 联合拓扑序把 clkgen 排在 clkdiv 前面，二者都在组合阶段之前执行
TEST(Clocking, ClockDividerDrivesDownstreamInSameTick) {
    Circuit c;
    auto *g = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::None));
    auto *div = c.addComponent(std::make_unique<ClockDivider>("div", 2));
    auto *ec = c.addComponent(std::make_unique<EdgeCounter>("ec", PinEdge::Rising));
    Net *clk = c.createNet("clk"), *dv = c.createNet("dv"), *cnt = c.createNet("cnt");
    c.connect(g, "clk", clk);
    c.connect(div, "clk", clk);
    c.connect(div, "out", dv);
    c.connect(ec, "clk", dv);
    c.connect(ec, "count", cnt);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();
    // 2 分频：第一个 clk 上升沿把 counter 推到 1、输出变 0，第二个沿回 0、输出变 1
    const uint8_t expect[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    uint32_t prev_count = 0;
    for (int i = 0; i < 8; ++i) {
        c.tick();
        uint8_t lv = c.getWire(dv->id())[0] & 1;
        uint32_t now = readCount(c, cnt);
        EXPECT_EQ(lv, expect[i]) << "第 " << i << " 拍分频输出不对";
        const bool should_rise = (i == 2 || i == 6);
        EXPECT_EQ(now, prev_count + (should_rise ? 1u : 0u)) << "第 " << i << " 拍计数不对";
        prev_count = now;
    }
}

// 时钟经组合逻辑（反相器）再驱动分频器：分频器必须读本拍的组合输出，
// 不能读到上一拍。联合拓扑序应排成 clkgen → 反相器 → clkdiv
TEST(Clocking, ClockDividerSeesCombinationalClockInSameTick) {
    Circuit c;
    auto *g = c.addComponent(std::make_unique<ClockGen>("g", 1, 1, PinEdge::None));
    auto *inv = c.addComponent(std::make_unique<UnaryGate>("inv", 1, true));
    auto *div = c.addComponent(std::make_unique<ClockDivider>("div", 2));
    auto *ec = c.addComponent(std::make_unique<EdgeCounter>("ec", PinEdge::Rising));
    Net *clk = c.createNet("clk"), *nclk = c.createNet("nclk"), *dv = c.createNet("dv"), *cnt = c.createNet("cnt");
    c.connect(g, "clk", clk);
    c.connect(inv, "in", clk);
    c.connect(inv, "out", nclk);
    c.connect(div, "clk", nclk);
    c.connect(div, "out", dv);
    c.connect(ec, "clk", dv);
    c.connect(ec, "count", cnt);
    auto e = c.compile();
    ASSERT_EQ(e.code, ErrorCode::OK) << e.message;
    c.init();
    // 反相时钟的上升沿出现在 tick2/4/6/8；分频输出在 tick1 就是高，
    // tick2 回 0、tick4 再变 1。若分频器晚一拍，tick2 会读到旧的 nclk=0，
    // 输出保持 1，序列立刻对不上
    const uint8_t expect[8] = {1, 0, 0, 1, 1, 0, 0, 1};
    uint32_t prev_count = 0;
    for (int i = 0; i < 8; ++i) {
        c.tick();
        uint8_t lv = c.getWire(dv->id())[0] & 1;
        uint32_t now = readCount(c, cnt);
        EXPECT_EQ(lv, expect[i]) << "第 " << i << " 拍分频输出不对（分频器相位晚了一拍？）";
        const bool should_rise = (i == 0 || i == 3 || i == 7);
        EXPECT_EQ(now, prev_count + (should_rise ? 1u : 0u)) << "第 " << i << " 拍计数不对";
        prev_count = now;
    }
}

// 两个时钟源经组合逻辑互相驱动：联合拓扑序排不出顺序，必须报错
TEST(Clocking, ClockSourceCycleThroughCombRejected) {
    Circuit c;
    auto *d1 = c.addComponent(std::make_unique<ClockDivider>("d1", 2));
    auto *d2 = c.addComponent(std::make_unique<ClockDivider>("d2", 2));
    auto *i1 = c.addComponent(std::make_unique<UnaryGate>("i1", 1, true));
    auto *i2 = c.addComponent(std::make_unique<UnaryGate>("i2", 1, true));
    Net *o1 = c.createNet("o1"), *o2 = c.createNet("o2"), *n1 = c.createNet("n1"), *n2 = c.createNet("n2");
    c.connect(d1, "out", o1);
    c.connect(i1, "in", o1);
    c.connect(i1, "out", n2);
    c.connect(d2, "clk", n2);
    c.connect(d2, "out", o2);
    c.connect(i2, "in", o2);
    c.connect(i2, "out", n1);
    c.connect(d1, "clk", n1);
    auto e = c.compile();
    EXPECT_NE(e.code, ErrorCode::OK);
    EXPECT_NE(e.message.find("时钟源之间存在环"), std::string::npos) << e.message;
}
