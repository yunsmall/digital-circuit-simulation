// PLIC 基本行为：优先级、使能、阈值、claim/complete、irq 输出
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/PLIC.h>

using namespace dsc;

namespace {

struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    PLIC *plic = nullptr;
    Net *clk = nullptr, *adr = nullptr, *dat_i = nullptr, *dat_o = nullptr;
    Net *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
    Net *src = nullptr, *ack = nullptr, *irq = nullptr;

    explicit Fixture(int sources = 4) {
        auto p = std::make_unique<PLIC>("plic0", sources);
        plic = p.get();
        circuit->addComponent(std::move(p));
        clk = circuit->createNet("clk");
        adr = circuit->createNet("adr");
        dat_i = circuit->createNet("dat_i");
        dat_o = circuit->createNet("dat_o");
        we = circuit->createNet("we");
        sel = circuit->createNet("sel");
        stb = circuit->createNet("stb");
        cyc = circuit->createNet("cyc");
        src = circuit->createNet("src");
        ack = circuit->createNet("ack");
        irq = circuit->createNet("irq");
        circuit->connect(plic, "clk", clk);
        circuit->connect(plic, "wb_adr_i", adr);
        circuit->connect(plic, "wb_dat_i", dat_i);
        circuit->connect(plic, "wb_we_i", we);
        circuit->connect(plic, "wb_sel_i", sel);
        circuit->connect(plic, "wb_stb_i", stb);
        circuit->connect(plic, "wb_cyc_i", cyc);
        circuit->connect(plic, "irq_src", src);
        circuit->connect(plic, "wb_dat_o", dat_o);
        circuit->connect(plic, "wb_ack_o", ack);
        circuit->connect(plic, "irq", irq);
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        circuit->setWire(clk->id(), {0});
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        circuit->setWire(we->id(), {0});
    }

    void cycle() {
        circuit->setWire(clk->id(), {0});
        circuit->tick();
        circuit->setWire(clk->id(), {1});
        circuit->tick();
    }

    void write(uint32_t addr, uint32_t val) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
                                    (uint8_t)((addr >> 24) & 0xFF)});
        circuit->setWire(dat_i->id(), {(uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF), (uint8_t)((val >> 16) & 0xFF),
                                      (uint8_t)((val >> 24) & 0xFF)});
        circuit->setWire(we->id(), {1});
        circuit->setWire(sel->id(), {0x0F});
        circuit->setWire(stb->id(), {1});
        circuit->setWire(cyc->id(), {1});
        for (int i = 0; i < 6; ++i)
            cycle();
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        for (int i = 0; i < 4; ++i)
            cycle();
    }

    uint32_t read(uint32_t addr) {
        circuit->setWire(adr->id(), {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
                                    (uint8_t)((addr >> 24) & 0xFF)});
        circuit->setWire(we->id(), {0});
        circuit->setWire(sel->id(), {0x0F});
        circuit->setWire(stb->id(), {1});
        circuit->setWire(cyc->id(), {1});
        for (int i = 0; i < 8; ++i)
            cycle();
        auto v = circuit->getWire(dat_o->id());
        circuit->setWire(stb->id(), {0});
        circuit->setWire(cyc->id(), {0});
        for (int i = 0; i < 4; ++i)
            cycle();
        return uint32_t(v[0]) | (uint32_t(v[1]) << 8) | (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 24);
    }

    void setSrc(uint32_t mask) {
        circuit->setWire(src->id(), {(uint8_t)(mask & 0xFF)});
        cycle();
        cycle(); // 时序输出到线网要再等一拍
    }
    bool irqHigh() { return circuit->getWire(irq->id())[0] != 0; }
};

} // namespace

TEST(PlicTest, PriorityAndEnableGate) {
    Fixture f;
    f.compile();
    f.write(0x4, 3);      // source 1 priority = 3
    f.write(0x2000 + 4, 1); // enable source 1
    EXPECT_FALSE(f.irqHigh());
    f.setSrc(0x01);        // source 1 到达
    EXPECT_TRUE(f.irqHigh()) << "使能且优先级高于阈值时应拉高中断";
    f.setSrc(0x00);
}

TEST(PlicTest, ThresholdBlocksInterrupt) {
    Fixture f;
    f.compile();
    f.write(0x4, 2);
    f.write(0x2000 + 4, 1);
    f.write(0x200000, 2); // 阈值 = 2，优先级 2 不高于阈值
    f.setSrc(0x01);
    EXPECT_FALSE(f.irqHigh());
}

TEST(PlicTest, ClaimAndComplete) {
    Fixture f;
    f.compile();
    f.write(0x4, 5);       // source 1 高优先级
    f.write(0x8, 3);       // source 2 低优先级
    f.write(0x2000 + 4, 1);
    f.write(0x2000 + 8, 1);
    f.setSrc(0x03);
    EXPECT_TRUE(f.irqHigh());
    EXPECT_EQ(f.read(0x200004), 1u) << "claim 应返回优先级最高的 source 1";
    f.setSrc(0x03);
    EXPECT_EQ(f.read(0x200004), 2u) << "source 1 在服务中，claim 应返回 source 2";
    f.write(0x200004, 1);  // complete source 1
    f.setSrc(0x01);
    EXPECT_EQ(f.read(0x200004), 1u) << "complete 后 source 1 可再次触发";
}
