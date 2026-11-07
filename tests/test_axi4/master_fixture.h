// AXI4 主设备适配器测试夹具：适配器 + 从桥 + Wishbone 存储器
#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/Axi4Bridge.h>
#include <dcs/components/platform/Axi4MasterAdapter.h>

namespace dsc::test {

struct MasterFixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Axi4MasterAdapter *master = nullptr;
    Axi4Bridge *bridge = nullptr;
    WishboneMemory *mem = nullptr;

    Net *clk, *rst;
    Net *mem_adr, *mem_wdat, *mem_we, *mem_sel, *mem_req, *mem_lock, *mem_ack, *mem_rdat;

    MasterFixture() {
        auto ap = std::make_unique<Axi4MasterAdapter>("master", 4);
        master = ap.get();
        c->addComponent(std::move(ap));
        auto bp = std::make_unique<Axi4Bridge>("bridge", 32, 32, 4);
        bridge = bp.get();
        c->addComponent(std::move(bp));
        auto mp = std::make_unique<WishboneMemory>("mem", 16, 32);
        mem = mp.get();
        c->addComponent(std::move(mp));

        auto mk = [&](const char *n) { return c->createNet(n); };
        clk = mk("clk");
        rst = mk("rst");
        mem_adr = mk("mem_adr");
        mem_wdat = mk("mem_wdat");
        mem_we = mk("mem_we");
        mem_sel = mk("mem_sel");
        mem_req = mk("mem_req");
        mem_lock = mk("mem_lock");
        mem_ack = mk("mem_ack");
        mem_rdat = mk("mem_rdat");

        using P = std::pair<const char *, Net *>;
        for (P p: {P{"clk", clk}, P{"rst", rst}, P{"mem_adr", mem_adr}, P{"mem_wdat", mem_wdat},
                   P{"mem_we", mem_we}, P{"mem_sel", mem_sel}, P{"mem_req", mem_req}, P{"mem_lock", mem_lock}})
            c->connect(master, p.first, p.second);

        // master 的 m_axi_* 与 bridge 的 s_axi_* 同名对接；连接方向由引脚决定
        for (const char *name: {"awid", "awaddr", "awlen", "awsize", "awburst", "awvalid", "wdata", "wstrb",
                                "wlast", "wvalid", "bready", "arid", "araddr", "arlen", "arsize", "arburst",
                                "arvalid", "rready", "awready", "wready", "arready", "bid", "bresp", "bvalid",
                                "rid", "rdata", "rresp", "rlast", "rvalid"}) {
            auto *net = c->createNet(name);
            c->connect(master, std::string("m_axi_") + name, net);
            c->connect(bridge, std::string("s_axi_") + name, net);
        }

        c->connect(master, "mem_ack", mem_ack);
        c->connect(master, "mem_rdat", mem_rdat);

        connectBridgeToMem();
    }

    void connectBridgeToMem() {
        auto mk = [&](const char *n) { return c->createNet(n); };
        auto *wb_adr = mk("wb_adr"), *wb_dat_o = mk("wb_dat_o"), *wb_we = mk("wb_we"), *wb_sel = mk("wb_sel");
        auto *wb_stb = mk("wb_stb"), *wb_cyc = mk("wb_cyc"), *wb_dat_i = mk("wb_dat_i"), *wb_ack = mk("wb_ack");
        using P = std::pair<const char *, Net *>;
        for (P p: {P{"wb_adr_o", wb_adr}, P{"wb_dat_o", wb_dat_o}, P{"wb_we_o", wb_we}, P{"wb_sel_o", wb_sel},
                   P{"wb_stb_o", wb_stb}, P{"wb_cyc_o", wb_cyc}, P{"wb_dat_i", wb_dat_i}, P{"wb_ack_i", wb_ack}})
            c->connect(bridge, p.first, p.second);
        for (P p: {P{"wb_adr_i", wb_adr}, P{"wb_dat_i", wb_dat_o}, P{"wb_we_i", wb_we}, P{"wb_sel_i", wb_sel},
                   P{"wb_stb_i", wb_stb}, P{"wb_cyc_i", wb_cyc}, P{"wb_dat_o", wb_dat_i}, P{"wb_ack_o", wb_ack}})
            c->connect(mem, p.first, p.second);
        c->connect(bridge, "clk", clk);
        c->connect(mem, "clk", clk);
        c->connect(bridge, "rst", rst);
    }

    void compile() {
        auto err = c->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
        c->setWire(rst->id(), 1);
        for (int i = 0; i < 3; i++) tick();
        c->setWire(rst->id(), 0);
        tick();
        c->setWire(mem_we->id(), 0);
        c->setWire(mem_req->id(), 0);
        c->setWire(mem_lock->id(), 0);
        c->setWire(mem_sel->id(), 0x0F);
        for (int i = 0; i < 2; i++) tick();
    }

    void tick() {
        c->setWire(clk->id(), 0);
        c->tick();
        c->setWire(clk->id(), 1);
        c->tick();
    }
    uint32_t get(Net *n) {
        auto v = c->getWire(n->id());
        uint32_t r = 0;
        for (int i = 0; i < 4; i++) r |= (uint32_t) v[i] << (8 * i);
        return r;
    }
    void set(Net *n, uint32_t v) {
        auto w = (size_t) (n->bit_width() + 7) / 8;
        if (w == 0) w = 1;
        std::vector<uint8_t> b(w);
        for (size_t i = 0; i < w; i++) b[i] = (uint8_t) ((v >> (8 * i)) & 0xFF);
        c->setWire(n->id(), b);
    }

    void write(uint32_t addr, uint32_t data, uint32_t sel = 0xF) {
        set(mem_adr, addr);
        set(mem_wdat, data);
        set(mem_sel, sel);
        set(mem_we, 1);
        set(mem_req, 1);
        set(mem_lock, 1);
        for (int i = 0; i < 300 && !get(mem_ack); i++) tick();
        ASSERT_TRUE(get(mem_ack)) << "写事务未收到 mem_ack";
        set(mem_req, 0);
        set(mem_lock, 0);
        for (int i = 0; i < 10 && get(mem_ack); i++) tick();
        EXPECT_EQ(get(mem_ack), 0u);
    }
    uint32_t read(uint32_t addr) {
        set(mem_adr, addr);
        set(mem_sel, 0xF);
        set(mem_we, 0);
        set(mem_req, 1);
        set(mem_lock, 1);
        for (int i = 0; i < 300 && !get(mem_ack); i++) tick();
        EXPECT_TRUE(get(mem_ack)) << "读事务未收到 mem_ack";
        const uint32_t v = get(mem_rdat);
        set(mem_req, 0);
        set(mem_lock, 0);
        for (int i = 0; i < 10 && get(mem_ack); i++) tick();
        EXPECT_EQ(get(mem_ack), 0u);
        return v;
    }
    uint32_t peek(uint32_t addr) {
        uint32_t v = 0;
        std::memcpy(&v, mem->writableMemPtr() + (addr & 0xFFFF), 4);
        return v;
    }
};

} // namespace dsc::test
