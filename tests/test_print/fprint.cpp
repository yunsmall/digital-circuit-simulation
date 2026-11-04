// FloatPrint 浮点打印元件测试：格式化输出可以按实例交给 TextSink
#include <cstring>
#include <dcs/Circuit.h>
#include <dcs/components/io/FloatPrint.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace dsc;

namespace {

void appendText(void *ctx, const Component *, const char *data, int len) {
    static_cast<std::string *>(ctx)->append(data, (size_t) len);
}

struct FloatPrintFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    FloatPrint *fp = nullptr;
    Net *in = nullptr;
    Net *clk = nullptr;
    std::string text;

    explicit FloatPrintFixture(int precision) {
        auto up = std::make_unique<FloatPrint>("fp", precision);
        fp = up.get();
        fp->setSink({&appendText}, &text);
        c->addComponent(std::move(up));
        in = c->createNet("in");
        clk = c->createNet("clk");
        c->connect(fp, "in", in);
        c->connect(fp, "clk", clk);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
    }

    void setBits(uint64_t bits, int bytes) {
        std::vector<uint8_t> data(bytes);
        for (int i = 0; i < bytes; i++)
            data[i] = (uint8_t) (bits >> (i * 8));
        c->setWire(in->id(), data);
    }

    void clockRise() {
        c->setWire(clk->id(), {0, 0});
        c->tick();
        c->setWire(clk->id(), {1, 0});
        c->tick();
    }
};

} // namespace

TEST(FloatPrintTest, SinkReceivesFormattedValue32) {
    FloatPrintFixture f(32);
    float v = 2.5f;
    uint32_t u;
    std::memcpy(&u, &v, 4);
    f.setBits(u, 4);
    f.clockRise();
    EXPECT_EQ(f.text, "[fp] 2.5\n");
}

TEST(FloatPrintTest, SinkReceivesFormattedValue64) {
    FloatPrintFixture f(64);
    double v = 3.14159;
    uint64_t u;
    std::memcpy(&u, &v, 8);
    f.setBits(u, 8);
    f.clockRise();
    EXPECT_EQ(f.text, "[fp] 3.14159\n");
}

TEST(FloatPrintTest, OnlyRisingEdgeEmits) {
    FloatPrintFixture f(64);
    double v = 1.5;
    uint64_t u;
    std::memcpy(&u, &v, 8);
    f.setBits(u, 8);
    f.clockRise();
    ASSERT_EQ(f.text, "[fp] 1.5\n");

    f.c->setWire(f.clk->id(), {1, 0});
    f.c->tick();
    EXPECT_EQ(f.text, "[fp] 1.5\n");
}

TEST(FloatPrintTest, InstancesUseTheirOwnSinks) {
    Circuit c;
    std::string a, b;
    auto up1 = std::make_unique<FloatPrint>("a", 32);
    auto up2 = std::make_unique<FloatPrint>("b", 32);
    auto *p1 = up1.get();
    auto *p2 = up2.get();
    p1->setSink({&appendText}, &a);
    p2->setSink({&appendText}, &b);
    c.addComponent(std::move(up1));
    c.addComponent(std::move(up2));
    Net *n1 = c.createNet("n1"), *n2 = c.createNet("n2"), *clk = c.createNet("clk");
    c.connect(p1, "in", n1);
    c.connect(p1, "clk", clk);
    c.connect(p2, "in", n2);
    c.connect(p2, "clk", clk);
    ASSERT_EQ(c.compile().code, ErrorCode::OK);
    c.init();

    float v1 = 1.25f, v2 = 4.5f;
    uint32_t u1, u2;
    std::memcpy(&u1, &v1, 4);
    std::memcpy(&u2, &v2, 4);
    c.setWire(n1->id(), u1);
    c.setWire(n2->id(), u2);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    EXPECT_EQ(a, "[a] 1.25\n");
    EXPECT_EQ(b, "[b] 4.5\n");
}

TEST(FloatPrintTest, JsonRoundTripKeepsPrecision) {
    Circuit c;
    auto *in = c.createNet("in"), *clk = c.createNet("clk");
    auto *p = c.addComponent(std::make_unique<FloatPrint>("f0", 64));
    c.connect(p, "in", in);
    c.connect(p, "clk", clk);

    std::string error;
    auto c2 = Circuit::fromJson(c.exportJson(), error);
    ASSERT_NE(c2, nullptr) << error;
    auto j = nlohmann::json::parse(c2->exportJson());
    ASSERT_EQ(j["components"].size(), 1u);
    EXPECT_EQ(j["components"][0]["type"], "fprint");
    EXPECT_EQ(j["components"][0]["params"]["precision"], "64");
}
