// Axi4Bridge 实现：AXI4 从设备状态机全部展开进 JIT，每个 beat 一次 Wishbone 事务。
#include "dcs/components/platform/Axi4Bridge.h"

#include <format>

#include "dcs/ComponentFactory.h"

namespace dsc {

namespace {
enum InIdx {
    I_CLK = 0, I_RST,
    I_AWID, I_AWADDR, I_AWLEN, I_AWSIZE, I_AWBURST, I_AWVALID,
    I_WDATA, I_WSTRB, I_WLAST, I_WVALID, I_BREADY,
    I_ARID, I_ARADDR, I_ARLEN, I_ARSIZE, I_ARBURST, I_ARVALID, I_RREADY,
    I_WB_DAT, I_WB_ACK
};
enum OutIdx {
    O_AWREADY = 0, O_WREADY, O_BID, O_BRESP, O_BVALID,
    O_ARREADY, O_RID, O_RDATA, O_RRESP, O_RLAST, O_RVALID,
    O_WB_ADR, O_WB_DAT, O_WB_WE, O_WB_SEL, O_WB_STB, O_WB_CYC
};

// 状态机：0 空闲，1 写 beat 等 ack，2 写响应，3 读发请求，4 读等 ack，5 读数据等 rready
enum St { ST_IDLE = 0, ST_WR_ACK = 1, ST_WR_RESP = 2, ST_RD_REQ = 3, ST_RD_ACK = 4, ST_RD_DATA = 5 };

constexpr int kMaxDataBytes = 16;
} // namespace

Axi4Bridge::Axi4Bridge(const std::string &name, int data_width, int addr_width, int id_width)
    : SequentialComponent(name, "axi4_bridge"), _data_width(data_width), _addr_width(addr_width),
      _id_width(id_width) {
    const int bytes = data_width / 8;
    setParam("data_width", std::to_string(data_width));
    setParam("addr_width", std::to_string(addr_width));
    setParam("id_width", std::to_string(id_width));

    addInput("clk", 1, PinEdge::Rising);
    addInput("rst", 1);
    addInput("s_axi_awid", id_width);
    addInput("s_axi_awaddr", addr_width);
    addInput("s_axi_awlen", 8);
    addInput("s_axi_awsize", 3);
    addInput("s_axi_awburst", 2);
    addInput("s_axi_awvalid", 1);
    addInput("s_axi_wdata", data_width);
    addInput("s_axi_wstrb", bytes);
    addInput("s_axi_wlast", 1);
    addInput("s_axi_wvalid", 1);
    addInput("s_axi_bready", 1);
    addInput("s_axi_arid", id_width);
    addInput("s_axi_araddr", addr_width);
    addInput("s_axi_arlen", 8);
    addInput("s_axi_arsize", 3);
    addInput("s_axi_arburst", 2);
    addInput("s_axi_arvalid", 1);
    addInput("s_axi_rready", 1);
    addInput("wb_dat_i", data_width);
    addInput("wb_ack_i", 1);

    addOutput("s_axi_awready", 1, false, true);
    addOutput("s_axi_wready", 1, false, true);
    addOutput("s_axi_bid", id_width, false, true);
    addOutput("s_axi_bresp", 2, false, true);
    addOutput("s_axi_bvalid", 1, false, true);
    addOutput("s_axi_arready", 1, false, true);
    addOutput("s_axi_rid", id_width, false, true);
    addOutput("s_axi_rdata", data_width, false, true);
    addOutput("s_axi_rresp", 2, false, true);
    addOutput("s_axi_rlast", 1, false, true);
    addOutput("s_axi_rvalid", 1, false, true);
    addOutput("wb_adr_o", addr_width, false, true);
    addOutput("wb_dat_o", data_width, false, true);
    addOutput("wb_we_o", 1, false, true);
    addOutput("wb_sel_o", bytes, false, true);
    addOutput("wb_stb_o", 1, false, true);
    addOutput("wb_cyc_o", 1, false, true);
}

Axi4Bridge::~Axi4Bridge() = default;

std::string Axi4Bridge::genStateDecl() const {
    const int id = _id;
    return std::format(
            // 当前写/读事务的地址结构
            "static uint32_t _ax4_cur_{0}, _ax4_start_{0}, _ax4_aligned_{0}, _ax4_wrap_{0};\n"
            "static uint32_t _ax4_nbytes_{0}, _ax4_beats_{0}, _ax4_beat_{0};\n"
            "static uint8_t _ax4_wdata_{0}[16], _ax4_rdata_{0}[16], _ax4_wb_dat_{0}[16];\n"
            "static uint16_t _ax4_wstrb_{0};\n"
            "static uint16_t _ax4_awid_{0}, _ax4_arid_{0};\n"
            "static uint8_t _ax4_burst_{0};\n"
            "static uint8_t _ax4_aw_got_{0}, _ax4_w_got_{0}, _ax4_wlast_{0};\n"
            "static uint8_t _ax4_bvalid_{0}, _ax4_bresp_{0};\n"
            "static uint16_t _ax4_bid_{0};\n"
            "static uint8_t _ax4_rvalid_{0}, _ax4_rlast_{0}, _ax4_rresp_{0};\n"
            "static uint16_t _ax4_rid_{0};\n"
            // 响应握手完成标志：置 VALID 的那一拍主设备还没看到 VALID，但它此时
            // 的 READY 值已经表明是否愿意接收；主设备看到 VALID 后会立刻撤 READY，
            // 只在下一拍看 READY 会把这次握手漏掉
            "static uint8_t _ax4_bdone_{0}, _ax4_rdone_{0};\n"
            "static uint8_t _ax4_wb_we_{0}, _ax4_wb_stb_{0}, _ax4_wb_cyc_{0};\n"
            "static uint8_t _ax4_st_{0};\n",
            id);
}

std::string Axi4Bridge::genInitCode() const {
    const int id = _id;
    return std::format(
            "    _ax4_cur_{0} = 0; _ax4_start_{0} = 0; _ax4_aligned_{0} = 0; _ax4_wrap_{0} = 0;\n"
            "    _ax4_nbytes_{0} = 1; _ax4_beats_{0} = 1; _ax4_beat_{0} = 0;\n"
            "    dcs_memset(_ax4_wdata_{0}, 0, 16); dcs_memset(_ax4_rdata_{0}, 0, 16);\n"
            "    dcs_memset(_ax4_wb_dat_{0}, 0, 16);\n"
            "    _ax4_wstrb_{0} = 0; _ax4_burst_{0} = 0; _ax4_awid_{0} = 0; _ax4_arid_{0} = 0;\n"
            "    _ax4_aw_got_{0} = 0; _ax4_w_got_{0} = 0; _ax4_wlast_{0} = 0;\n"
            "    _ax4_bvalid_{0} = 0; _ax4_bresp_{0} = 0; _ax4_bid_{0} = 0;\n"
            "    _ax4_rvalid_{0} = 0; _ax4_rlast_{0} = 0; _ax4_rresp_{0} = 0; _ax4_rid_{0} = 0;\n"
            "    _ax4_bdone_{0} = 0; _ax4_rdone_{0} = 0;\n"
            "    _ax4_wb_we_{0} = 0; _ax4_wb_stb_{0} = 0; _ax4_wb_cyc_{0} = 0; _ax4_st_{0} = 0;\n",
            id);
}

// 生成"事务内下一个 beat 地址"的计算。AXI 规范 A3.4.1：
// 首拍用 Start_Address，之后用 Aligned_Address + (N-1)*Number_Bytes，
// WRAP 越过上界时回到回绕边界；FIXED 地址不变。
std::string Axi4Bridge::genAdvanceCode() const {
    const int id = _id;
    return std::format(
            "        if (_ax4_burst_{0} == 0) {{\n"
            "            _ax4_cur_{0} = _ax4_start_{0};\n"
            "        }} else {{\n"
            "            _ax4_cur_{0} = _ax4_aligned_{0} + _ax4_beat_{0} * _ax4_nbytes_{0};\n"
            "            if (_ax4_burst_{0} == 2) {{\n"
            "                uint32_t _total = _ax4_nbytes_{0} * _ax4_beats_{0};\n"
            "                if (_ax4_cur_{0} >= _ax4_wrap_{0} + _total) _ax4_cur_{0} = _ax4_cur_{0} - _total;\n"
            "            }}\n"
            "        }}\n",
            id);
}

std::string Axi4Bridge::genFuncDef_seq() const {
    const int id = _id;
    const int bytes = _data_width / 8;
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    auto read = [&](int pin, int width, const char *var) {
        const int net_w = inputs()[pin]->net() ? inputs()[pin]->net()->bit_width() : 0;
        return gen_read_wire(inputs()[pin]->netId(), width, net_w, var);
    };

    std::string c;
    c += std::format("// Axi4Bridge {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    const bool _clk_pos = " + genGetWireEdge(I_CLK, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";

    c += "    " + read(I_RST, 1, "_rst") + "\n";
    c += "    " + read(I_AWADDR, _addr_width, "_awaddr") + "\n";
    c += "    " + read(I_AWLEN, 8, "_awlen") + "\n";
    c += "    " + read(I_AWSIZE, 3, "_awsize") + "\n";
    c += "    " + read(I_AWBURST, 2, "_awburst") + "\n";
    c += "    " + read(I_AWID, _id_width, "_awid") + "\n";
    c += "    " + read(I_AWVALID, 1, "_awvalid") + "\n";
    c += "    " + read(I_WSTRB, bytes, "_wstrb_in") + "\n";
    c += "    " + read(I_WLAST, 1, "_wlast_in") + "\n";
    c += "    " + read(I_WVALID, 1, "_wvalid") + "\n";
    c += "    " + read(I_BREADY, 1, "_bready") + "\n";
    c += "    " + read(I_ARADDR, _addr_width, "_araddr") + "\n";
    c += "    " + read(I_ARLEN, 8, "_arlen") + "\n";
    c += "    " + read(I_ARSIZE, 3, "_arsize") + "\n";
    c += "    " + read(I_ARBURST, 2, "_arburst") + "\n";
    c += "    " + read(I_ARID, _id_width, "_arid") + "\n";
    c += "    " + read(I_ARVALID, 1, "_arvalid") + "\n";
    c += "    " + read(I_RREADY, 1, "_rready") + "\n";
    c += "    " + read(I_WB_DAT, _data_width, "_wb_dati") + "\n";
    c += "    " + read(I_WB_ACK, 1, "_wb_ack") + "\n";

    c += "    uint8_t _awready = 0, _wready = 0, _arready = 0;\n";
    // 复位：撤销所有在途事务。AXI 要求主从一起复位，单独复位桥会让等响应的一侧
    // 永远等下去；这里至少保证桥自身不会带着半截事务继续跑
    c += std::format(
            "    if (_rst) {{\n"
            "        _ax4_aw_got_{0} = 0; _ax4_w_got_{0} = 0; _ax4_wlast_{0} = 0;\n"
            "        _ax4_bvalid_{0} = 0; _ax4_rvalid_{0} = 0; _ax4_rlast_{0} = 0;\n"
            "        _ax4_bdone_{0} = 0; _ax4_rdone_{0} = 0;\n"
            "        _ax4_wb_we_{0} = 0; _ax4_wb_stb_{0} = 0; _ax4_wb_cyc_{0} = 0;\n"
            "        _ax4_beat_{0} = 0; _ax4_st_{0} = 0;\n"
            "        goto _ax4_out_{0};\n"
            "    }}\n",
            id);

    // 接收 AW：锁存 burst 参数并算出回绕边界
    c += std::format(
            "    if (_ax4_st_{0} == 0 && !_ax4_aw_got_{0} && _awvalid) {{\n"
            "        _ax4_awid_{0} = _awid; _ax4_start_{0} = _awaddr;\n"
            "        _ax4_nbytes_{0} = 1u << _awsize;\n"
            "        _ax4_beats_{0} = (uint32_t)_awlen + 1u;\n"
            "        _ax4_burst_{0} = _awburst; _ax4_aligned_{0} = _awaddr & ~(_ax4_nbytes_{0} - 1u);\n"
            "        _ax4_wrap_{0} = _awaddr & ~(_ax4_nbytes_{0} * _ax4_beats_{0} - 1u);\n"
            "        _ax4_cur_{0} = _awaddr; _ax4_beat_{0} = 0;\n"
            "        _ax4_aw_got_{0} = 1; _awready = 1;\n"
            "    }}\n"
            "    if (_ax4_st_{0} == 0 && !_ax4_w_got_{0} && _wvalid && _ax4_aw_got_{0}) {{\n"
            "        dcs_memcpy(_ax4_wdata_{0}, _w[{1}], {2});\n"
            "        _ax4_wstrb_{0} = _wstrb_in; _ax4_wlast_{0} = _wlast_in;\n"
            "        _ax4_w_got_{0} = 1; _wready = 1;\n"
            "    }}\n",
            id, nid(I_WDATA), bytes);
    // 读地址只在完全没有写事务时接受，保证单 outstanding、读写不交错
    c += std::format(
            "    if (_ax4_st_{0} == 0 && !_ax4_aw_got_{0} && !_ax4_w_got_{0} && _arvalid) {{\n"
            "        _ax4_arid_{0} = _arid; _ax4_start_{0} = _araddr;\n"
            "        _ax4_nbytes_{0} = 1u << _arsize;\n"
            "        _ax4_beats_{0} = (uint32_t)_arlen + 1u;\n"
            "        _ax4_burst_{0} = _arburst; _ax4_aligned_{0} = _araddr & ~(_ax4_nbytes_{0} - 1u);\n"
            "        _ax4_wrap_{0} = _araddr & ~(_ax4_nbytes_{0} * _ax4_beats_{0} - 1u);\n"
            "        _ax4_cur_{0} = _araddr; _ax4_beat_{0} = 0;\n"
            "        _arready = 1; _ax4_st_{0} = 3;\n"
            "    }}\n",
            id);

    // 写通道状态机
    c += std::format(
            "    if (_ax4_st_{0} == 0 && _ax4_aw_got_{0} && _ax4_w_got_{0}) {{\n"
            "        dcs_memcpy(_ax4_wb_dat_{0}, _ax4_wdata_{0}, {1});\n"
            "        _ax4_wb_we_{0} = 1; _ax4_wb_stb_{0} = 1; _ax4_wb_cyc_{0} = 1;\n"
            "        _ax4_st_{0} = 1;\n"
            "    }} else if (_ax4_st_{0} == 1) {{\n"
            "        if (_wb_ack) {{\n"
            "            _ax4_wb_stb_{0} = 0; _ax4_wb_cyc_{0} = 0;\n"
            "            if (_ax4_wlast_{0}) {{\n"
            "                _ax4_bvalid_{0} = 1; _ax4_bresp_{0} = 0; _ax4_bid_{0} = _ax4_awid_{0};\n"
            "                _ax4_bdone_{0} = _bready;\n"
            "                _ax4_st_{0} = 2;\n"
            "            }} else {{\n"
            "                _ax4_beat_{0} = _ax4_beat_{0} + 1u;\n"
            "                if (_ax4_beat_{0} >= _ax4_beats_{0}) _ax4_beat_{0} = _ax4_beats_{0} - 1u;\n",
            id, bytes);
    c += genAdvanceCode();
    c += std::format(
            "                _ax4_w_got_{0} = 0; _ax4_st_{0} = 0;\n"
            "            }}\n"
            "        }}\n"
            "    }} else if (_ax4_st_{0} == 2) {{\n"
            "        if (_ax4_bdone_{0} || _bready) {{\n"
            "            _ax4_bvalid_{0} = 0; _ax4_bdone_{0} = 0;\n"
            "            _ax4_aw_got_{0} = 0; _ax4_w_got_{0} = 0; _ax4_wlast_{0} = 0;\n"
            "            _ax4_beat_{0} = 0; _ax4_st_{0} = 0;\n"
            "        }}\n"
            "    }}\n",
            id);

    // 读通道状态机：每 beat 一次 Wishbone 读，最后一拍 RLAST
    c += std::format(
            "    if (_ax4_st_{0} == 3) {{\n"
            "        _ax4_wb_we_{0} = 0; _ax4_wb_stb_{0} = 1; _ax4_wb_cyc_{0} = 1;\n"
            "        _ax4_st_{0} = 4;\n"
            "    }} else if (_ax4_st_{0} == 4) {{\n"
            "        if (_wb_ack) {{\n"
            "            _ax4_wb_stb_{0} = 0; _ax4_wb_cyc_{0} = 0;\n"
            "            dcs_memcpy(_ax4_rdata_{0}, &_wb_dati, {1});\n"
            "            _ax4_rvalid_{0} = 1; _ax4_rresp_{0} = 0; _ax4_rid_{0} = _ax4_arid_{0};\n"
            "            _ax4_rlast_{0} = (_ax4_beat_{0} + 1u >= _ax4_beats_{0}) ? 1u : 0u;\n"
            "            _ax4_rdone_{0} = _rready;\n"
            "            _ax4_st_{0} = 5;\n"
            "        }}\n"
            "    }} else if (_ax4_st_{0} == 5) {{\n"
            "        if (_ax4_rdone_{0} || _rready) {{\n"
            "            _ax4_rvalid_{0} = 0; _ax4_rdone_{0} = 0;\n"
            "            if (_ax4_rlast_{0}) {{\n"
            "                _ax4_rlast_{0} = 0; _ax4_beat_{0} = 0; _ax4_st_{0} = 0;\n"
            "            }} else {{\n"
            "                _ax4_beat_{0} = _ax4_beat_{0} + 1u;\n",
            id, bytes);
    c += genAdvanceCode();
    c += std::format(
            "                _ax4_st_{0} = 3;\n"
            "            }}\n"
            "        }}\n"
            "    }}\n",
            id);

    // 输出：所有握手与数据信号每拍重写，状态变量保持整个周期
    c += std::format("_ax4_out_{}:\n    ;\n", id);
    c += "    " + genOutputWrite(O_AWREADY, "_awready", 1) + "\n";
    c += "    " + genOutputWrite(O_WREADY, "_wready", 1) + "\n";
    c += "    " + genOutputWrite(O_BID, "_ax4_bid_" + std::to_string(id), _id_width) + "\n";
    c += "    " + genOutputWrite(O_BRESP, "_ax4_bresp_" + std::to_string(id), 2) + "\n";
    c += "    " + genOutputWrite(O_BVALID, "_ax4_bvalid_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_ARREADY, "_arready", 1) + "\n";
    c += "    " + genOutputWrite(O_RID, "_ax4_rid_" + std::to_string(id), _id_width) + "\n";
    c += "    " + genOutputWrite(O_RDATA, "_ax4_rdata_" + std::to_string(id), _data_width) + "\n";
    c += "    " + genOutputWrite(O_RRESP, "_ax4_rresp_" + std::to_string(id), 2) + "\n";
    c += "    " + genOutputWrite(O_RLAST, "_ax4_rlast_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_RVALID, "_ax4_rvalid_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_WB_ADR, "_ax4_cur_" + std::to_string(id), _addr_width) + "\n";
    c += "    " + genOutputWrite(O_WB_DAT, "_ax4_wb_dat_" + std::to_string(id), _data_width) + "\n";
    c += "    " + genOutputWrite(O_WB_WE, "_ax4_wb_we_" + std::to_string(id), 1) + "\n";
    // SEL：写事务用 WSTRB；读事务用全 1，让从设备返回整字再由 AXI 数据通道选字节。
    // 选通位数等于数据字节数（128 位端口是 16 位），不能按 uint8_t 暂存
    c += "    { uint16_t _sel = _ax4_wb_we_" + std::to_string(id) + " ? _ax4_wstrb_" + std::to_string(id) +
         " : (uint16_t) " + std::format("0x{:X}", (1u << bytes) - 1u) + ";\n";
    c += "      " + genOutputWrite(O_WB_SEL, "_sel", bytes) + " }\n";
    c += "    " + genOutputWrite(O_WB_STB, "_ax4_wb_stb_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_WB_CYC, "_ax4_wb_cyc_" + std::to_string(id), 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Axi4Bridge::clone(const std::string &new_name) const {
    return std::make_unique<Axi4Bridge>(new_name, _data_width, _addr_width, _id_width);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
        {"axi4_bridge",
         {{"data_width", "32", {"32", "64", "128"}}, {"addr_width", "32"}, {"id_width", "4"}},
         {"clk", "rst", "s_axi_awid", "s_axi_awaddr", "s_axi_awlen", "s_axi_awsize", "s_axi_awburst", "s_axi_awvalid",
          "s_axi_wdata", "s_axi_wstrb", "s_axi_wlast", "s_axi_wvalid", "s_axi_bready", "s_axi_arid", "s_axi_araddr",
          "s_axi_arlen", "s_axi_arsize", "s_axi_arburst", "s_axi_arvalid", "s_axi_rready", "wb_dat_i", "wb_ack_i"},
         {"s_axi_awready", "s_axi_wready", "s_axi_bid", "s_axi_bresp", "s_axi_bvalid", "s_axi_arready", "s_axi_rid",
          "s_axi_rdata", "s_axi_rresp", "s_axi_rlast", "s_axi_rvalid", "wb_adr_o", "wb_dat_o", "wb_we_o", "wb_sel_o",
          "wb_stb_o", "wb_cyc_o"},
         dsc::ComponentCategory::Bus, "AXI4 从设备到 Wishbone 主设备，支持 INCR/WRAP/FIXED burst"},
        [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            auto gi = [&](const char *k, int d) {
                auto it = p.find(k);
                return it == p.end() ? d : std::stoi(it->second);
            };
            return std::make_unique<dsc::Axi4Bridge>(n, gi("data_width", 32), gi("addr_width", 32), gi("id_width", 4));
        });
} // namespace
