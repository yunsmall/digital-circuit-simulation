// RegBridge：Wishbone 事务 ↔ 通用寄存器接口
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/platform/RegBridge.h>

using namespace dsc;

namespace {

struct RbFixture {
    std::unique_ptr<Circuit> c;
    RegBridge *rb = nullptr;
    Net *clk, *adr, *dat, *we, *sel, *stb, *cyc, *rdat, *ready;
    Net *o_dat, *o_ack, *o_addr, *o_wdat, *o_we, *o_en;

    RbFixture() {
        c = std::make_unique<Circuit>();
        auto q = std::make_unique<RegBridge>("rb0");
        rb = q.get();
        c->addComponent(std::move(q));
        clk = c->createNet("clk"); adr = c->createNet("adr"); dat = c->createNet("dat");
        we = c->createNet("we"); sel = c->createNet("sel"); stb = c->createNet("stb");
        cyc = c->createNet("cyc"); rdat = c->createNet("rdat"); ready = c->createNet("ready");
        o_dat = c->createNet("o_dat"); o_ack = c->createNet("o_ack"); o_addr = c->createNet("o_addr");
        o_wdat = c->createNet("o_wdat"); o_we = c->createNet("o_we"); o_en = c->createNet("o_en");
        for (auto [pin, net] : {std::pair{"clk", clk}, {"wb_adr_i", adr}, {"wb_dat_i", dat},
                                {"wb_we_i", we}, {"wb_sel_i", sel}, {"wb_stb_i", stb},
                                {"wb_cyc_i", cyc}, {"rdata", rdat}, {"ready", ready},
                                {"wb_dat_o", o_dat}, {"wb_ack_o", o_ack}, {"addr", o_addr},
                                {"wdata", o_wdat}, {"we", o_we}, {"en", o_en}})
            c->connect(rb, pin, net);
        auto err = c->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
    }

    void ticks(int n) {
        for (int i = 0; i < n; i++) c->tick();
    }
    static void set32(Net *n, Circuit *c, uint32_t v) {
        c->setWire(n->id(), {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)});
    }
    uint32_t get32(Net *n) const {
        auto v = c->getWire(n->id());
        return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
    }
    void begin(uint32_t a, uint32_t d, bool write, bool rdy = true) {
        set32(adr, c.get(), a);
        set32(dat, c.get(), d);
        c->setWire(we->id(), {(uint8_t) (write ? 1 : 0)});
        c->setWire(sel->id(), {0x0F});
        c->setWire(ready->id(), {(uint8_t) (rdy ? 1 : 0)});
        c->setWire(stb->id(), {1});
        c->setWire(cyc->id(), {1});
        c->tick(); // 发起拍：en 拉高，addr/wdata 锁存
    }
    void end() {
        c->setWire(stb->id(), {0});
        ticks(2);
        c->setWire(cyc->id(), {0});
        ticks(1);
    }
    int en() const { return c->getWire(o_en->id())[0] & 1; }
    int ack() const { return c->getWire(o_ack->id())[0] & 1; }
};

} // namespace

TEST(RegBridgeTest, WriteTransactionDrivesRegInterface) {
    RbFixture f;
    f.begin(0x04, 0xDEADBEEF, true);
    EXPECT_EQ(f.en(), 1) << "事务起始应拉 en";
    EXPECT_EQ(f.c->getWire(f.o_addr->id())[0], 0x04) << "addr 低 8 位没有透传";
    EXPECT_EQ(f.get32(f.o_wdat), 0xDEADBEEFu);
    EXPECT_EQ(f.c->getWire(f.o_we->id())[0] & 1, 1);
    f.end();
    EXPECT_EQ(f.en(), 0) << "en 只能在事务起始拉一拍";
}

TEST(RegBridgeTest, ReadTransactionReturnsDataOnAck) {
    RbFixture f;
    f.c->setWire(f.rdat->id(), {0x78, 0x56, 0x34, 0x12});
    f.begin(0x08, 0, false);
    f.ticks(4);
    EXPECT_EQ(f.ack(), 1) << "ready 有效后必须回 ack";
    EXPECT_EQ(f.get32(f.o_dat), 0x12345678u) << "rdata 没有返回给主设备";
    f.end();
    EXPECT_EQ(f.ack(), 0) << "事务结束 ack 必须撤销";
}

TEST(RegBridgeTest, EnIsSinglePulseEvenIfStbHeld) {
    RbFixture f;
    f.begin(0x00, 0, true);
    // en 的脉冲在 begin 的两拍里已经过去，之后 stb 一直保持也不该再拉高
    for (int i = 0; i < 10; i++) {
        f.ticks(1);
        EXPECT_EQ(f.en(), 0) << "stb 保持期间不能重复发起事务";
    }
    f.end();
}

TEST(RegBridgeTest, TwoSequentialTransactions) {
    RbFixture f;
    f.begin(0x01, 0xAAAA, true);
    f.end();
    f.begin(0x02, 0xBBBB, true);
    EXPECT_EQ(f.c->getWire(f.o_addr->id())[0], 0x02);
    EXPECT_EQ(f.get32(f.o_wdat), 0xBBBBu);
    f.end();
}

TEST(RegBridgeTest, AckWaitsForReady) {
    RbFixture f;
    f.begin(0x00, 0, false, false); // ready 先不给
    f.ticks(5);
    EXPECT_EQ(f.ack(), 0) << "ready 未到不应回 ack";
    f.c->setWire(f.ready->id(), {1});
    f.ticks(3);
    EXPECT_EQ(f.ack(), 1) << "ready 到达后应回 ack";
    f.end();
}
