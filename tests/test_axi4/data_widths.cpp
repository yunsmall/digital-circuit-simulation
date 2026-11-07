// AXI4 桥的数据位宽参数：64/128 位总线上 WSTRB、WDATA、RDATA、SEL 的宽度与内容。
// WishboneMemory 只支持 8/16/32 位，所以这里用测试台直接驱动桥的 Wishbone 侧。
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/platform/Axi4Bridge.h>

#include "fixture.h"

using namespace dsc;
using dsc::test::Axi4Fixture;

namespace {

struct BridgeOnly {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    Axi4Bridge *bridge = nullptr;
    int bytes = 4;

    Net *clk, *rst;
    Net *awid, *awaddr, *awlen, *awsize, *awburst, *awvalid;
    Net *wdata, *wstrb, *wlast, *wvalid, *bready;
    Net *arid, *araddr, *arlen, *arsize, *arburst, *arvalid, *rready;
    Net *wb_dat_i, *wb_ack_i;
    Net *awready, *wready, *bid, *bresp, *bvalid;
    Net *arready, *rid, *rdata, *rresp, *rlast, *rvalid;
    Net *wb_adr_o, *wb_dat_o, *wb_we_o, *wb_sel_o, *wb_stb_o, *wb_cyc_o;

    explicit BridgeOnly(int data_width) : bytes(data_width / 8) {
        auto bp = std::make_unique<Axi4Bridge>("axi", data_width, 32, 4);
        bridge = bp.get();
        c->addComponent(std::move(bp));

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
        for (int i = 0; i < 4; i++) r |= (uint32_t) v[i] << (8 * i);
        return r;
    }
    std::vector<uint8_t> getBytes(Net *n, int count) {
        auto v = c->getWire(n->id());
        return std::vector<uint8_t>(v.begin(), v.begin() + count);
    }
    void set(Net *n, uint32_t v) {
        auto w = (size_t) (n->bit_width() + 7) / 8;
        if (w == 0) w = 1;
        std::vector<uint8_t> b(w);
        for (size_t i = 0; i < w; i++) b[i] = (uint8_t) ((v >> (8 * i)) & 0xFF);
        c->setWire(n->id(), b);
    }
    // 写整个数据通道（低位在前），用于 64/128 位 WDATA 和 WSTRB
    void setBytes(Net *n, const std::vector<uint8_t> &v) {
        std::vector<uint8_t> b((size_t) (n->bit_width() + 7) / 8, 0);
        for (size_t i = 0; i < v.size() && i < b.size(); i++) b[i] = v[i];
        c->setWire(n->id(), b);
    }
    static std::vector<uint8_t> pattern(int n, uint8_t seed) {
        std::vector<uint8_t> v((size_t) n);
        for (int i = 0; i < n; i++) v[i] = (uint8_t) (seed + i * 0x11);
        return v;
    }
};

} // namespace

TEST(Axi4WidthTest, EndToEnd64WithWideMemory) {
    Axi4Fixture f(64);
    f.compile();
    f.write(0x100, {0x11223344u});
    EXPECT_EQ(f.peek(0x100), 0x11223344u);
    EXPECT_EQ(f.read(0x100, 1)[0], 0x11223344u);
    // 相邻的 8 字节字不能互相干扰
    f.write(0x108, {0x55667788u});
    EXPECT_EQ(f.peek(0x108), 0x55667788u);
    EXPECT_EQ(f.read(0x108, 1)[0], 0x55667788u);
    EXPECT_EQ(f.peek(0x100), 0x11223344u);
}

TEST(Axi4WidthTest, NarrowTransferOn64BitBusEndToEnd) {
    Axi4Fixture f(64);
    f.compile();
    // 2 字节传输落在字内第 2、3 字节通道
    f.write(0x102, {0xABCDu}, 1);
    EXPECT_EQ(f.peek(0x100) & 0xFFFF0000u, 0xABCD0000u);
    EXPECT_EQ(f.peek(0x100) & 0xFFFFu, 0u);
    auto v = f.read(0x102, 1, 1);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ((v[0] >> 16) & 0xFFFF, 0xABCDu);
}

TEST(Axi4WidthTest, Write64UsesFullStrobeAndData) {
    BridgeOnly f(64);
    f.compile();
    const auto wd = BridgeOnly::pattern(8, 0x10);
    f.set(f.awaddr, 0x100);
    f.set(f.awlen, 0);
    f.set(f.awsize, 3); // 8 字节
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);

    f.setBytes(f.wdata, wd);
    f.set(f.wstrb, 0xFF);
    f.set(f.wlast, 1);
    f.set(f.wvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.wready); i++) f.tick();
    ASSERT_TRUE(f.get(f.wready));
    f.set(f.wvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.wb_stb_o); i++) f.tick();
    ASSERT_TRUE(f.get(f.wb_stb_o));
    EXPECT_EQ(f.get(f.wb_adr_o), 0x100u);
    EXPECT_EQ(f.get(f.wb_we_o), 1u);
    EXPECT_EQ(f.get(f.wb_sel_o), 0xFFu) << "64 位端口的 SEL 必须覆盖 8 个字节";
    EXPECT_EQ(f.getBytes(f.wb_dat_o, 8), wd);

    // 从设备应答，事务结束
    f.set(f.wb_ack_i, 1);
    for (int i = 0; i < 50 && !f.get(f.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.bvalid));
    EXPECT_EQ(f.get(f.bid), 0u);
    f.set(f.wb_ack_i, 0);
}

TEST(Axi4WidthTest, Read64ReturnsFullData) {
    BridgeOnly f(64);
    f.compile();
    const auto rd = BridgeOnly::pattern(8, 0x80);
    f.set(f.araddr, 0x200);
    f.set(f.arlen, 0);
    f.set(f.arsize, 3);
    f.set(f.arburst, 1);
    f.set(f.arvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.arready); i++) f.tick();
    ASSERT_TRUE(f.get(f.arready));
    f.set(f.arvalid, 0);

    for (int i = 0; i < 50 && !f.get(f.wb_stb_o); i++) f.tick();
    ASSERT_TRUE(f.get(f.wb_stb_o));
    EXPECT_EQ(f.get(f.wb_we_o), 0u);
    EXPECT_EQ(f.get(f.wb_sel_o), 0xFFu) << "读事务的 SEL 必须是整字全 1";
    f.setBytes(f.wb_dat_i, rd);
    f.set(f.wb_ack_i, 1);
    for (int i = 0; i < 50 && !f.get(f.rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.rvalid));
    EXPECT_EQ(f.getBytes(f.rdata, 8), rd);
    EXPECT_EQ(f.get(f.rlast), 1u);
    f.set(f.wb_ack_i, 0);
}

TEST(Axi4WidthTest, Write128UsesSixteenByteStrobe) {
    BridgeOnly f(128);
    f.compile();
    const auto wd = BridgeOnly::pattern(16, 0x30);
    f.set(f.awaddr, 0x300);
    f.set(f.awlen, 0);
    f.set(f.awsize, 4); // 16 字节
    f.set(f.awburst, 1);
    f.set(f.awvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.awready); i++) f.tick();
    ASSERT_TRUE(f.get(f.awready));
    f.set(f.awvalid, 0);

    f.setBytes(f.wdata, wd);
    f.setBytes(f.wstrb, std::vector<uint8_t>(16, 0xFF));
    f.set(f.wlast, 1);
    f.set(f.wvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.wready); i++) f.tick();
    ASSERT_TRUE(f.get(f.wready));
    f.set(f.wvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.wb_stb_o); i++) f.tick();
    ASSERT_TRUE(f.get(f.wb_stb_o));
    EXPECT_EQ(f.get(f.wb_sel_o), 0xFFFFu) << "128 位端口的 SEL 必须覆盖 16 个字节";
    EXPECT_EQ(f.getBytes(f.wb_dat_o, 16), wd);
    EXPECT_EQ(f.get(f.wb_adr_o), 0x300u);

    f.set(f.wb_ack_i, 1);
    for (int i = 0; i < 50 && !f.get(f.bvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.bvalid));
    f.set(f.wb_ack_i, 0);
}

TEST(Axi4WidthTest, Read128ReturnsFullData) {
    BridgeOnly f(128);
    f.compile();
    const auto rd = BridgeOnly::pattern(16, 0x40);
    f.set(f.araddr, 0x400);
    f.set(f.arlen, 0);
    f.set(f.arsize, 4);
    f.set(f.arburst, 1);
    f.set(f.arvalid, 1);
    for (int i = 0; i < 50 && !f.get(f.arready); i++) f.tick();
    ASSERT_TRUE(f.get(f.arready));
    f.set(f.arvalid, 0);
    for (int i = 0; i < 50 && !f.get(f.wb_stb_o); i++) f.tick();
    ASSERT_TRUE(f.get(f.wb_stb_o));
    f.setBytes(f.wb_dat_i, rd);
    f.set(f.wb_ack_i, 1);
    for (int i = 0; i < 50 && !f.get(f.rvalid); i++) f.tick();
    ASSERT_TRUE(f.get(f.rvalid));
    EXPECT_EQ(f.getBytes(f.rdata, 16), rd);
    f.set(f.wb_ack_i, 0);
}
