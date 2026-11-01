// GPIO：DIR/OUT/IN 寄存器 + gpio_in/gpio_out 引脚
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/io/Gpio.h>

using namespace dsc;

namespace {

struct GpioFixture {
    std::unique_ptr<Circuit> c;
    Gpio *g = nullptr;
    Net *clk, *addr, *wdata, *we, *en, *rdata, *ready, *gin, *gout;

    GpioFixture() {
        c = std::make_unique<Circuit>();
        auto p = std::make_unique<Gpio>("g0");
        g = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        addr = c->createNet("addr");
        wdata = c->createNet("wdata");
        we = c->createNet("we");
        en = c->createNet("en");
        rdata = c->createNet("rdata");
        ready = c->createNet("ready");
        gin = c->createNet("gin");
        gout = c->createNet("gout");
        c->connect(g, "clk", clk);
        c->connect(g, "addr", addr);
        c->connect(g, "wdata", wdata);
        c->connect(g, "we", we);
        c->connect(g, "en", en);
        c->connect(g, "rdata", rdata);
        c->connect(g, "ready", ready);
        c->connect(g, "gpio_in", gin);
        c->connect(g, "gpio_out", gout);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
    }

    void ticks(int n) {
        for (int i = 0; i < n; i++) c->tick();
    }
    void write(uint32_t a, uint32_t d) {
        c->setWire(addr->id(), (uint8_t) a);
        c->setWire(wdata->id(), {(uint8_t) d, (uint8_t) (d >> 8), (uint8_t) (d >> 16), (uint8_t) (d >> 24)});
        c->setWire(we->id(), 1);
        c->setWire(en->id(), 1);
        ticks(2);
        c->setWire(en->id(), 0);
        ticks(1);
    }
    uint32_t read(uint32_t a) {
        c->setWire(addr->id(), (uint8_t) a);
        c->setWire(we->id(), 0);
        c->setWire(en->id(), 1);
        ticks(3);
        auto v = c->getWire(rdata->id());
        c->setWire(en->id(), 0);
        ticks(1);
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    uint32_t out() const {
        auto v = c->getWire(gout->id());
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
};

} // namespace

TEST(GpioTest, DirectionMasksOutput) {
    GpioFixture f;
    f.write(0x04, 0xA5A5A5A5); // OUT
    f.write(0x00, 0x0000FFFF); // DIR：只有低 16 位是输出
    f.ticks(3);
    EXPECT_EQ(f.out(), 0x0000A5A5u) << "非输出方向的位不应驱动到 gpio_out";
}

TEST(GpioTest, ReadsInputPins) {
    GpioFixture f;
    f.c->setWire(f.gin->id(), {0x11, 0x22, 0x33, 0x44});
    f.ticks(3);
    EXPECT_EQ(f.read(0x08), 0x44332211u) << "IN 寄存器没有反映 gpio_in 引脚";
}

TEST(GpioTest, RegistersReadBack) {
    GpioFixture f;
    f.write(0x00, 0x00FF00FF);
    f.write(0x04, 0x12345678);
    EXPECT_EQ(f.read(0x00), 0x00FF00FFu);
    EXPECT_EQ(f.read(0x04), 0x12345678u);
}

TEST(GpioTest, WritingReadOnlyInputRegisterHasNoEffect) {
    GpioFixture f;
    f.c->setWire(f.gin->id(), {0x01, 0, 0, 0});
    f.write(0x08, 0xFFFFFFFF); // IN 是只读的，写它不能改输入引脚
    EXPECT_EQ(f.read(0x08), 1u) << "写 IN 寄存器不应影响输入值";
}

TEST(GpioTest, UnknownAddressReadsZero) {
    GpioFixture f;
    EXPECT_EQ(f.read(0x7C), 0u);
    EXPECT_EQ(f.read(0xFF), 0u);
}

TEST(GpioTest, EnStaysHighWritesOnce) {
    GpioFixture f;
    f.write(0x04, 0x11111111);
    // 握手按 en 上升沿触发：在同一个 en 高电平期间换数据，不该重复写
    f.c->setWire(f.addr->id(), 0x04);
    f.c->setWire(f.wdata->id(), {0x22, 0x22, 0x22, 0x22});
    f.c->setWire(f.we->id(), 1);
    f.c->setWire(f.en->id(), 1);
    f.ticks(2);
    f.c->setWire(f.wdata->id(), {0x33, 0x33, 0x33, 0x33});
    f.ticks(2); // en 仍为高，这次不该写入
    f.c->setWire(f.en->id(), 0);
    f.ticks(1);
    EXPECT_EQ(f.read(0x04), 0x22222222u) << "en 持续为高时不应重复写入";
}

TEST(GpioTest, ReadyFollowsEnable) {
    GpioFixture f;
    f.c->setWire(f.en->id(), 0);
    f.ticks(2);
    EXPECT_EQ(f.c->getWire(f.ready->id())[0], 0);
    f.c->setWire(f.en->id(), 1);
    f.ticks(2);
    EXPECT_EQ(f.c->getWire(f.ready->id())[0], 1);
}

TEST(GpioTest, InitClearsRegistersAndOutput) {
    GpioFixture f;
    f.write(0x00, 0xFFFFFFFF);
    f.write(0x04, 0xFFFFFFFF);
    f.c->init();
    EXPECT_EQ(f.read(0x00), 0u);
    EXPECT_EQ(f.read(0x04), 0u);
    EXPECT_EQ(f.out(), 0u);
}
