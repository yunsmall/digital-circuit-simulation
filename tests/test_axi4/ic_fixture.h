// AXI4 互连测试夹具：2 主 2 从，每从口一个桥 + Wishbone 存储器
#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/Axi4Bridge.h>
#include <dcs/components/platform/Axi4Interconnect.h>

namespace dsc::test {

// Masters 参数化：单主口的互连走 extra_bits=0 的分支，和双主口行为不同
template <int Masters>
struct IcFixtureT {
    static constexpr int kMasters = Masters;
    static constexpr int kSlaves = 2;
    // 从口看到的 ID 宽度 = 主口 ID 宽度 + 附加的主口编号位数
    static constexpr int kExtraBits = (Masters <= 1) ? 0 : (Masters <= 2) ? 1 : (Masters <= 4) ? 2 : 3;

    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Axi4Interconnect *ic = nullptr;
    Axi4Bridge *br[kSlaves] = {nullptr, nullptr};
    WishboneMemory *mem[kSlaves] = {nullptr, nullptr};

    Net *clk, *rst;
    struct MasterNets {
        Net *awid, *awaddr, *awlen, *awsize, *awburst, *awvalid;
        Net *wdata, *wstrb, *wlast, *wvalid, *bready;
        Net *arid, *araddr, *arlen, *arsize, *arburst, *arvalid, *rready;
        Net *awready, *wready, *bid, *bresp, *bvalid;
        Net *arready, *rid, *rdata, *rresp, *rlast, *rvalid;
    } mn[kMasters];

    IcFixtureT() {
        auto ip = std::make_unique<Axi4Interconnect>("ic", "0:4096,8192:4096", kMasters, 4);
        ic = ip.get();
        c->addComponent(std::move(ip));
        for (int s = 0; s < kSlaves; s++) {
            auto bp = std::make_unique<Axi4Bridge>("br" + std::to_string(s), 32, 32, 4 + kExtraBits);
            br[s] = bp.get();
            c->addComponent(std::move(bp));
            auto mp = std::make_unique<WishboneMemory>("mem" + std::to_string(s), 12, 32);
            mem[s] = mp.get();
            c->addComponent(std::move(mp));
        }

        auto mk = [&](const char *n) { return c->createNet(n); };
        clk = mk("clk");
        rst = mk("rst");
        using P = std::pair<std::string, Net *>;

        for (int m = 0; m < kMasters; m++) {
            auto &n = mn[m];
            const std::string p = "s" + std::to_string(m) + "_axi_";
            n.awid = mk((p + "awid").c_str());
            n.awaddr = mk((p + "awaddr").c_str());
            n.awlen = mk((p + "awlen").c_str());
            n.awsize = mk((p + "awsize").c_str());
            n.awburst = mk((p + "awburst").c_str());
            n.awvalid = mk((p + "awvalid").c_str());
            n.wdata = mk((p + "wdata").c_str());
            n.wstrb = mk((p + "wstrb").c_str());
            n.wlast = mk((p + "wlast").c_str());
            n.wvalid = mk((p + "wvalid").c_str());
            n.bready = mk((p + "bready").c_str());
            n.arid = mk((p + "arid").c_str());
            n.araddr = mk((p + "araddr").c_str());
            n.arlen = mk((p + "arlen").c_str());
            n.arsize = mk((p + "arsize").c_str());
            n.arburst = mk((p + "arburst").c_str());
            n.arvalid = mk((p + "arvalid").c_str());
            n.rready = mk((p + "rready").c_str());
            n.awready = mk((p + "awready").c_str());
            n.wready = mk((p + "wready").c_str());
            n.bid = mk((p + "bid").c_str());
            n.bresp = mk((p + "bresp").c_str());
            n.bvalid = mk((p + "bvalid").c_str());
            n.arready = mk((p + "arready").c_str());
            n.rid = mk((p + "rid").c_str());
            n.rdata = mk((p + "rdata").c_str());
            n.rresp = mk((p + "rresp").c_str());
            n.rlast = mk((p + "rlast").c_str());
            n.rvalid = mk((p + "rvalid").c_str());
            for (P pp: {P{p + "awid", n.awid}, P{p + "awaddr", n.awaddr},
                        P{p + "awlen", n.awlen}, P{p + "awsize", n.awsize}, P{p + "awburst", n.awburst},
                        P{p + "awvalid", n.awvalid}, P{p + "wdata", n.wdata}, P{p + "wstrb", n.wstrb},
                        P{p + "wlast", n.wlast}, P{p + "wvalid", n.wvalid}, P{p + "bready", n.bready},
                        P{p + "arid", n.arid}, P{p + "araddr", n.araddr}, P{p + "arlen", n.arlen},
                        P{p + "arsize", n.arsize}, P{p + "arburst", n.arburst}, P{p + "arvalid", n.arvalid},
                        P{p + "rready", n.rready}, P{"clk", clk}, P{"rst", rst}})
                c->connect(ic, pp.first, pp.second);
            for (P pp: {P{p + "awready", n.awready}, P{p + "wready", n.wready}, P{p + "bid", n.bid},
                        P{p + "bresp", n.bresp}, P{p + "bvalid", n.bvalid}, P{p + "arready", n.arready},
                        P{p + "rid", n.rid}, P{p + "rdata", n.rdata}, P{p + "rresp", n.rresp},
                        P{p + "rlast", n.rlast}, P{p + "rvalid", n.rvalid}})
                c->connect(ic, pp.first, pp.second);
        }

        for (int s = 0; s < kSlaves; s++) {
            const std::string p = "m" + std::to_string(s) + "_axi_";
            auto net = [&](const char *suffix) { return mk((p + suffix).c_str()); };
            // IC 从口的 18 个输出和 11 个输入各建一根线，桥的同名口接同一根线
            Net *awid = net("awid"), *awaddr = net("awaddr"), *awlen = net("awlen"), *awsize = net("awsize"),
                 *awburst = net("awburst"), *awvalid = net("awvalid");
            Net *wdata = net("wdata"), *wstrb = net("wstrb"), *wlast = net("wlast"), *wvalid = net("wvalid");
            Net *bready = net("bready");
            Net *arid = net("arid"), *araddr = net("araddr"), *arlen = net("arlen"), *arsize = net("arsize"),
                 *arburst = net("arburst"), *arvalid = net("arvalid"), *rready = net("rready");
            Net *awready = net("awready"), *wready = net("wready"), *bid = net("bid"), *bresp = net("bresp"),
                 *bvalid = net("bvalid"), *arready = net("arready"), *rid = net("rid"), *rdata = net("rdata"),
                 *rresp = net("rresp"), *rlast = net("rlast"), *rvalid = net("rvalid");

            for (P pp: {P{p + "awready", awready}, P{p + "wready", wready}, P{p + "bid", bid},
                        P{p + "bresp", bresp}, P{p + "bvalid", bvalid}, P{p + "arready", arready},
                        P{p + "rid", rid}, P{p + "rdata", rdata}, P{p + "rresp", rresp},
                        P{p + "rlast", rlast}, P{p + "rvalid", rvalid}})
                c->connect(ic, pp.first, pp.second);
            for (P pp: {P{p + "awid", awid}, P{p + "awaddr", awaddr}, P{p + "awlen", awlen},
                        P{p + "awsize", awsize}, P{p + "awburst", awburst}, P{p + "awvalid", awvalid},
                        P{p + "wdata", wdata}, P{p + "wstrb", wstrb}, P{p + "wlast", wlast},
                        P{p + "wvalid", wvalid}, P{p + "bready", bready}, P{p + "arid", arid},
                        P{p + "araddr", araddr}, P{p + "arlen", arlen}, P{p + "arsize", arsize},
                        P{p + "arburst", arburst}, P{p + "arvalid", arvalid}, P{p + "rready", rready}})
                c->connect(ic, pp.first, pp.second);

            for (P pp: {P{"s_axi_awid", awid}, P{"s_axi_awaddr", awaddr}, P{"s_axi_awlen", awlen},
                        P{"s_axi_awsize", awsize}, P{"s_axi_awburst", awburst}, P{"s_axi_awvalid", awvalid},
                        P{"s_axi_wdata", wdata}, P{"s_axi_wstrb", wstrb}, P{"s_axi_wlast", wlast},
                        P{"s_axi_wvalid", wvalid}, P{"s_axi_bready", bready}, P{"s_axi_arid", arid},
                        P{"s_axi_araddr", araddr}, P{"s_axi_arlen", arlen}, P{"s_axi_arsize", arsize},
                        P{"s_axi_arburst", arburst}, P{"s_axi_arvalid", arvalid}, P{"s_axi_rready", rready},
                        P{"clk", clk}, P{"rst", rst}})
                c->connect(br[s], pp.first, pp.second);
            for (P pp: {P{"s_axi_awready", awready}, P{"s_axi_wready", wready}, P{"s_axi_bid", bid},
                        P{"s_axi_bresp", bresp}, P{"s_axi_bvalid", bvalid}, P{"s_axi_arready", arready},
                        P{"s_axi_rid", rid}, P{"s_axi_rdata", rdata}, P{"s_axi_rresp", rresp},
                        P{"s_axi_rlast", rlast}, P{"s_axi_rvalid", rvalid}})
                c->connect(br[s], pp.first, pp.second);

            // 从桥 → Wishbone 存储器
            auto *wb_adr = net("wb_adr"), *wb_dat_o = net("wb_dat_o"), *wb_we = net("wb_we"),
                 *wb_sel = net("wb_sel"), *wb_stb = net("wb_stb"), *wb_cyc = net("wb_cyc"),
                 *wb_dat_i = net("wb_dat_i"), *wb_ack = net("wb_ack");
            for (P pp: {P{"wb_adr_o", wb_adr}, P{"wb_dat_o", wb_dat_o}, P{"wb_we_o", wb_we}, P{"wb_sel_o", wb_sel},
                        P{"wb_stb_o", wb_stb}, P{"wb_cyc_o", wb_cyc}, P{"wb_dat_i", wb_dat_i}, P{"wb_ack_i", wb_ack}})
                c->connect(br[s], pp.first, pp.second);
            for (P pp: {P{"wb_adr_i", wb_adr}, P{"wb_dat_i", wb_dat_o}, P{"wb_we_i", wb_we}, P{"wb_sel_i", wb_sel},
                        P{"wb_stb_i", wb_stb}, P{"wb_cyc_i", wb_cyc}, P{"wb_dat_o", wb_dat_i}, P{"wb_ack_o", wb_ack},
                        P{"clk", clk}})
                c->connect(mem[s], pp.first, pp.second);
        }
    }

    void compile() {
        auto err = c->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        c->init();
        c->setWire(rst->id(), 1);
        for (int m = 0; m < kMasters; m++) {
            c->setWire(mn[m].bready->id(), 1);
            c->setWire(mn[m].rready->id(), 1);
        }
        for (int i = 0; i < 4; i++) tick();
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

    // 主口 m 发一笔写 burst。窄传输缺省按地址把数据和选通放进对应字节通道。
    void write(int m, uint32_t addr, const std::vector<uint32_t> &beats, uint8_t size = 2, uint8_t burst = 1,
               uint8_t id = 0, uint32_t strobe = 0xFFFFFFFFu) {
        auto &n = mn[m];
        set(n.awid, id);
        set(n.awaddr, addr);
        set(n.awlen, (uint32_t) beats.size() - 1);
        set(n.awsize, size);
        set(n.awburst, burst);
        set(n.awvalid, 1);
        for (int i = 0; i < 200 && !get(n.awready); i++) tick();
        ASSERT_TRUE(get(n.awready)) << "主口 " << m << " AWREADY 未拉高";
        set(n.awvalid, 0);
        for (size_t i = 0; i < beats.size(); i++) {
            uint32_t wd = beats[i], st = strobe;
            if (size < 2 && strobe == 0xFFFFFFFFu) {
                const uint32_t beat_bytes = 1u << size;
                const uint32_t lane = (addr + (uint32_t) i * beat_bytes) % 4u;
                wd = beats[i] << (8u * lane);
                st = ((1u << beat_bytes) - 1u) << lane;
            }
            set(n.wdata, wd);
            set(n.wstrb, st);
            set(n.wlast, i + 1 == beats.size());
            set(n.wvalid, 1);
            for (int k = 0; k < 300 && !get(n.wready); k++) tick();
            ASSERT_TRUE(get(n.wready)) << "主口 " << m << " WREADY 未拉高，beat " << i;
            set(n.wvalid, 0);
            tick();
        }
        for (int i = 0; i < 500 && !get(n.bvalid); i++) tick();
        ASSERT_TRUE(get(n.bvalid)) << "主口 " << m << " BVALID 未拉高";
        EXPECT_EQ(get(n.bid), (uint32_t) id);
        tick();
        EXPECT_EQ(get(n.bvalid), 0u);
    }

    std::vector<uint32_t> read(int m, uint32_t addr, uint32_t beats, uint8_t size = 2, uint8_t burst = 1,
                               uint8_t id = 0) {
        auto &n = mn[m];
        set(n.arid, id);
        set(n.araddr, addr);
        set(n.arlen, beats - 1);
        set(n.arsize, size);
        set(n.arburst, burst);
        set(n.arvalid, 1);
        for (int i = 0; i < 200 && !get(n.arready); i++) tick();
        EXPECT_TRUE(get(n.arready)) << "主口 " << m << " ARREADY 未拉高";
        set(n.arvalid, 0);
        std::vector<uint32_t> out;
        for (uint32_t i = 0; i < beats; i++) {
            for (int k = 0; k < 500 && !get(n.rvalid); k++) tick();
            EXPECT_TRUE(get(n.rvalid)) << "主口 " << m << " RVALID 未拉高，beat " << i;
            out.push_back(get(n.rdata));
            EXPECT_EQ(get(n.rid), (uint32_t) id);
            EXPECT_EQ(get(n.rlast), (i + 1 == beats) ? 1u : 0u);
            tick();
        }
        for (int i = 0; i < 10 && get(n.rvalid); i++) tick();
        EXPECT_EQ(get(n.rvalid), 0u);
        return out;
    }

    uint32_t peek(int s, uint32_t addr) {
        uint32_t v = 0;
        std::memcpy(&v, mem[s]->writableMemPtr() + (addr & 0xFFF), 4);
        return v;
    }
};

using IcFixture = IcFixtureT<2>;

} // namespace dsc::test
