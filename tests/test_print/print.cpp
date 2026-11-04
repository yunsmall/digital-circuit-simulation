// Print 打印元件测试：默认 stdout 之外，每个实例可以单独绑定 TextSink
#include <dcs/Circuit.h>
#include <dcs/components/io/Print.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

using namespace dsc;

namespace {

void appendText(void *ctx, const Component *owner, const char *data, int len) {
    static_cast<std::string *>(ctx)->append(data, (size_t) len);
}

struct PrintFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Print *p = nullptr;
    Net *in = nullptr;
    Net *clk = nullptr;
    Net *en = nullptr;
    std::string text;

    explicit PrintFixture(int bw = 8, bool ascii = false, bool connect_en = false) {
        auto up = std::make_unique<Print>("p", bw, ascii);
        p = up.get();
        p->setSink({&appendText}, &text);
        c->addComponent(std::move(up));
        in = c->createNet("in");
        clk = c->createNet("clk");
        c->connect(p, "in", in);
        c->connect(p, "clk", clk);
        if (connect_en) {
            en = c->createNet("en");
            c->connect(p, "en", en);
        }
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    // 一个完整的时钟上升沿
    void clockRise() {
        c->setWire(clk->id(), {0, 0});
        c->tick();
        c->setWire(clk->id(), {1, 0});
        c->tick();
    }
};

} // namespace

TEST(PrintTest, HexModeSendsFormattedLineToSink) {
    PrintFixture f(8);
    f.c->setWire(f.in->id(), {0xAB, 0});
    f.clockRise();
    EXPECT_EQ(f.text, "[p] AB\n");
}

TEST(PrintTest, AsciiModeSendsRawChars) {
    PrintFixture f(8, true);
    f.c->setWire(f.in->id(), {0x41, 0}); // 'A'
    f.clockRise();
    EXPECT_EQ(f.text, "A");
}

// 没接 en 时按恒使能处理：每个 clk 上升沿都要输出，不能只在 init 后第一拍输出
TEST(PrintTest, AsciiWithoutEnPrintsEveryClockRising) {
    PrintFixture f(8, true);
    f.c->setWire(f.in->id(), {0x41, 0});
    f.clockRise();
    f.c->setWire(f.in->id(), {0x42, 0});
    f.clockRise();
    EXPECT_EQ(f.text, "AB");
}

// 接了 en 时它是 valid 选通：和 clk 不同步也要在 en 上升沿交付，且只交一次
TEST(PrintTest, AsciiWithEnUsesValidStrobeNotClock) {
    PrintFixture f(8, true, true);
    f.c->setWire(f.en->id(), {0, 0});
    f.c->setWire(f.clk->id(), {0, 0});
    f.c->tick();

    f.c->setWire(f.in->id(), {0x58, 0}); // 'X'
    f.c->setWire(f.en->id(), {1, 0});    // valid 拉高，此时 clk 仍是低
    f.c->tick();
    EXPECT_EQ(f.text, "X");

    f.c->setWire(f.clk->id(), {1, 0}); // 只有 clk 上升，不应重复输出
    f.c->tick();
    EXPECT_EQ(f.text, "X");

    f.c->setWire(f.in->id(), {0x59, 0}); // 'Y'
    f.c->setWire(f.en->id(), {0, 0});
    f.c->tick();
    f.c->setWire(f.en->id(), {1, 0});
    f.c->tick();
    EXPECT_EQ(f.text, "XY");
}

TEST(PrintTest, OnlyRisingEdgeEmits) {
    PrintFixture f(8);
    f.c->setWire(f.in->id(), {0x11, 0});
    f.clockRise();
    EXPECT_EQ(f.text, "[p] 11\n");

    f.c->setWire(f.in->id(), {0x22, 0});
    f.c->setWire(f.clk->id(), {1, 0});
    f.c->tick(); // 时钟保持高，不该再打印
    EXPECT_EQ(f.text, "[p] 11\n");
}

// 每个实例的 sink 独立：两个 print 各写各的字符串
TEST(PrintTest, InstancesUseTheirOwnSinks) {
    Circuit c;
    std::string a, b;
    auto up1 = std::make_unique<Print>("p1", 8);
    auto up2 = std::make_unique<Print>("p2", 8);
    auto *p1 = up1.get();
    auto *p2 = up2.get();
    p1->setSink({&appendText}, &a);
    p2->setSink({&appendText}, &b);
    c.addComponent(std::move(up1));
    c.addComponent(std::move(up2));
    Net *n1 = c.createNet("n1"), *n2 = c.createNet("n2");
    Net *clk = c.createNet("clk");
    c.connect(p1, "in", n1);
    c.connect(p1, "clk", clk);
    c.connect(p2, "in", n2);
    c.connect(p2, "clk", clk);
    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    c.init();

    c.setWire(n1->id(), {0x01, 0});
    c.setWire(n2->id(), {0x02, 0});
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();

    EXPECT_EQ(a, "[p1] 01\n");
    EXPECT_EQ(b, "[p2] 02\n");
}

TEST(PrintTest, ClearSinkStopsDeliveryButOutputStillWorks) {
    PrintFixture f(8);
    f.c->setWire(f.in->id(), {0x33, 0});
    f.clockRise();
    ASSERT_EQ(f.text, "[p] 33\n");

    f.p->clearSink();
    f.c->setWire(f.in->id(), {0x44, 0});
    f.clockRise();
    EXPECT_EQ(f.text, "[p] 33\n") << "解绑后不应再进回调";
}

TEST(PrintTest, JsonRoundTripKeepsParams) {
    Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk");
    auto *p = c.addComponent(std::make_unique<Print>("p0", 12, true));
    c.connect(p, "in", in);
    c.connect(p, "clk", clk);

    std::string error;
    auto c2 = Circuit::fromJson(c.exportJson(), error);
    ASSERT_NE(c2, nullptr) << error;
    auto j = nlohmann::json::parse(c2->exportJson());
    ASSERT_EQ(j["components"].size(), 1u);
    EXPECT_EQ(j["components"][0]["type"], "print");
    EXPECT_EQ(j["components"][0]["params"]["bit_width"], "12");
    EXPECT_EQ(j["components"][0]["params"]["ascii"], "1");
}
