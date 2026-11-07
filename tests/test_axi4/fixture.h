// AXI4 从设备桥测试夹具：桥 + Wishbone 存储器，以及一个最小 AXI4 主设备驱动
#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/Axi4Bridge.h>

namespace dsc::test {

struct Axi4Fixture {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Axi4Bridge *bridge = nullptr;
    WishboneMemory *mem = nullptr;

    Net *clk, *rst;
    Net *awid, *awaddr, *awlen, *awsize, *awburst, *awvalid;
    Net *wdata, *wstrb, *wlast, *wvalid, *bready;
    Net *arid, *araddr, *arlen, *arsize, *arburst, *arvalid, *rready;
    Net *wb_dat_i, *wb_ack_i;
    Net *awready, *wready, *bid, *bresp, *bvalid;
    Net *arready, *rid, *rdata, *rresp, *rlast, *rvalid;
    Net *wb_adr_o, *wb_dat_o, *wb_we_o, *wb_sel_o, *wb_stb_o, *wb_cyc_o;

    explicit Axi4Fixture(int data_width = 32) {
        auto bp = std::make_unique<Axi4Bridge>("axi", data_width, 32, 4);
        bridge = bp.get();
        c->addComponent(std::move(bp));
        auto mp = std::make_unique<WishboneMemory>("mem", 16, data_width);
        mem = mp.get();
        c->addComponent(std::move(mp));

        auto mk = [&](const char *n) { return c->createNet(n); };
        clk = mk("clk");
        rst = mk("rst");
        awid = mk("awid");
        awaddr = mk("awaddr");
        awlen = mk("awlen");
        awsize = mk("awsize");
        awburst = mk("awburst");
        awvalid = mk("awvalid");
        wdata = mk("wdata");
        wstrb = mk("wstrb");
        wlast = mk("wlast");
        wvalid = mk("wvalid");
        bready = mk("bready");
        arid = mk("arid");
        araddr = mk("araddr");
        arlen = mk("arlen");
        arsize = mk("arsize");
        arburst = mk("arburst");
        arvalid = mk("arvalid");
        rready = mk("rready");
        wb_dat_i = mk("wb_dat_i");
        wb_ack_i = mk("wb_ack_i");
        awready = mk("awready");
        wready = mk("wready");
        bid = mk("bid");
        bresp = mk("bresp");
        bvalid = mk("bvalid");
        arready = mk("arready");
        rid = mk("rid");
        rdata = mk("rdata");
        rresp = mk("rresp");
        rlast = mk("rlast");
        rvalid = mk("rvalid");
        wb_adr_o = mk("wb_adr_o");
        wb_dat_o = mk("wb_dat_o");
        wb_we_o = mk("wb_we_o");
        wb_sel_o = mk("wb_sel_o");
        wb_stb_o = mk("wb_stb_o");
        wb_cyc_o = mk("wb_cyc_o");

        using P = std::pair<const char *, Net *>;
        for (P p: {P{"clk", clk}, P{"rst", rst}, P{"s_axi_awid", awid}, P{"s_axi_awaddr", awaddr},
                   P{"s_axi_awlen", awlen}, P{"s_axi_awsize", awsize}, P{"s_axi_awburst", awburst},
                   P{"s_axi_awvalid", awvalid}, P{"s_axi_wdata", wdata}, P{"s_axi_wstrb", wstrb},
                   P{"s_axi_wlast", wlast}, P{"s_axi_wvalid", wvalid}, P{"s_axi_bready", bready},
                   P{"s_axi_arid", arid}, P{"s_axi_araddr", araddr}, P{"s_axi_arlen", arlen},
                   P{"s_axi_arsize", arsize}, P{"s_axi_arburst", arburst}, P{"s_axi_arvalid", arvalid},
                   P{"s_axi_rready", rready}, P{"wb_dat_i", wb_dat_i}, P{"wb_ack_i", wb_ack_i}})
            c->connect(bridge, p.first, p.second);
        for (P p: {P{"s_axi_awready", awready}, P{"s_axi_wready", wready}, P{"s_axi_bid", bid},
                   P{"s_axi_bresp", bresp}, P{"s_axi_bvalid", bvalid}, P{"s_axi_arready", arready},
                   P{"s_axi_rid", rid}, P{"s_axi_rdata", rdata}, P{"s_axi_rresp", rresp},
                   P{"s_axi_rlast", rlast}, P{"s_axi_rvalid", rvalid}, P{"wb_adr_o", wb_adr_o},
                   P{"wb_dat_o", wb_dat_o}, P{"wb_we_o", wb_we_o}, P{"wb_sel_o", wb_sel_o},
                   P{"wb_stb_o", wb_stb_o}, P{"wb_cyc_o", wb_cyc_o}})
            c->connect(bridge, p.first, p.second);

        for (P p: {P{"clk", clk}, P{"wb_adr_i", wb_adr_o}, P{"wb_dat_i", wb_dat_o}, P{"wb_we_i", wb_we_o},
                   P{"wb_sel_i", wb_sel_o}, P{"wb_stb_i", wb_stb_o}, P{"wb_cyc_i", wb_cyc_o},
                   P{"wb_dat_o", wb_dat_i}, P{"wb_ack_o", wb_ack_i}})
            c->connect(mem, p.first, p.second);
    }

    void compile() {
        auto err = c->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
        c->setWire(rst->id(), 1);
        c->setWire(bready->id(), 1);
        c->setWire(rready->id(), 1);
        for (int i = 0; i < 3; i++) tick();
        c->setWire(rst->id(), 0);
        tick();
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
        for (int i = 0; i < 4; i++)
            r |= (uint32_t) v[i] << (8 * i);
        return r;
    }
    void set(Net *n, uint32_t v) {
        auto w = (size_t) (n->bit_width() + 7) / 8;
        if (w == 0) w = 1;
        std::vector<uint8_t> b(w);
        for (size_t i = 0; i < w; i++) b[i] = (uint8_t) ((v >> (8 * i)) & 0xFF);
        c->setWire(n->id(), b);
    }

    // 写一笔 burst。aw_first=false 时 AW 与第一个 W beat 同时抬高，覆盖
    // AXI4 允许写数据先于（或同拍于）地址到达的情况。strobe 缺省时按窄传输
    // 自动把数据和选通放到地址对应的字节通道上。
    void write(uint32_t addr, const std::vector<uint32_t> &beats, uint8_t size = 2, uint8_t burst = 1,
               uint8_t id = 0, bool aw_first = true, uint32_t strobe = 0xFFFFFFFFu,
               const std::vector<uint32_t> &strobes = {}) {
        const uint32_t len = (uint32_t) beats.size() - 1;
        set(awid, id);
        set(awaddr, addr);
        set(awlen, len);
        set(awsize, size);
        set(awburst, burst);
        set(awvalid, 1);
        bool aw_done = false;
        if (aw_first) {
            for (int i = 0; i < 100 && !get(awready); i++) tick();
            ASSERT_TRUE(get(awready)) << "AWREADY 未拉高";
            set(awvalid, 0);
            aw_done = true;
        }
        for (size_t i = 0; i < beats.size(); i++) {
            uint32_t wd = beats[i], st = strobe;
            if (!strobes.empty()) st = strobes[i];
            else if (size < 2 && strobe == 0xFFFFFFFFu) {
                const uint32_t beat_bytes = 1u << size;
                const uint32_t lane = (addr + (uint32_t) i * beat_bytes) % 4u;
                wd = beats[i] << (8u * lane);
                st = ((1u << beat_bytes) - 1u) << lane;
            }
            set(wdata, wd);
            set(wstrb, st);
            set(wlast, i + 1 == beats.size());
            set(wvalid, 1);
            bool w_done = false;
            for (int k = 0; k < 200 && !w_done; k++) {
                tick();
                if (!aw_done && get(awready)) {
                    aw_done = true;
                    set(awvalid, 0);
                }
                if (get(wready)) w_done = true;
            }
            ASSERT_TRUE(w_done) << "WREADY 未拉高";
            set(wvalid, 0);
            tick();
        }
        for (int i = 0; i < 200 && !get(bvalid); i++) tick();
        ASSERT_TRUE(get(bvalid)) << "BVALID 未拉高";
        EXPECT_EQ(get(bresp), 0u);
        EXPECT_EQ(get(bid), (uint32_t) id);
        tick();
        EXPECT_EQ(get(bvalid), 0u);
    }

    std::vector<uint32_t> read(uint32_t addr, uint32_t beats, uint8_t size = 2, uint8_t burst = 1,
                               uint8_t id = 0) {
        set(arid, id);
        set(araddr, addr);
        set(arlen, beats - 1);
        set(arsize, size);
        set(arburst, burst);
        set(arvalid, 1);
        for (int i = 0; i < 100 && !get(arready); i++) tick();
        EXPECT_TRUE(get(arready)) << "ARREADY 未拉高";
        set(arvalid, 0);
        std::vector<uint32_t> out;
        for (uint32_t i = 0; i < beats; i++) {
            for (int k = 0; k < 200 && !get(rvalid); k++) tick();
            EXPECT_TRUE(get(rvalid)) << "RVALID 未拉高，beat " << i;
            out.push_back(get(rdata));
            EXPECT_EQ(get(rresp), 0u);
            EXPECT_EQ(get(rid), (uint32_t) id);
            EXPECT_EQ(get(rlast), (i + 1 == beats) ? 1u : 0u);
            tick();
        }
        for (int i = 0; i < 10 && get(rvalid); i++) tick();
        EXPECT_EQ(get(rvalid), 0u);
        return out;
    }

    uint32_t peek(uint32_t addr) {
        uint32_t v = 0;
        std::memcpy(&v, mem->writableMemPtr() + (addr & 0xFFFF), 4);
        return v;
    }
};

} // namespace dsc::test
