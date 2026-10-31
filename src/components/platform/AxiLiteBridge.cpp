// AxiLiteBridge — AXI4-Lite 从设备到 Wishbone 主设备的桥
// 状态机、握手与应答全部展开在 JIT 里，每拍没有跨模块调用。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/AxiLiteBridge.h"

#include <format>

namespace dsc {

namespace {
enum InIdx {
    I_AWADDR = 0, I_AWVALID, I_WDATA, I_WSTRB, I_WVALID, I_BREADY,
    I_ARADDR, I_ARVALID, I_RREADY, I_WB_DAT, I_WB_ACK
};
enum OutIdx {
    O_AWREADY = 0, O_WREADY, O_BRESP, O_BVALID, O_ARREADY, O_RDATA, O_RRESP, O_RVALID,
    O_WB_ADR, O_WB_DAT, O_WB_WE, O_WB_SEL, O_WB_STB, O_WB_CYC
};

// 状态机取值
enum St { ST_IDLE = 0, ST_WRITE_WAIT = 1, ST_WRITE_RESP = 2, ST_READ_WAIT = 3, ST_READ_RESP = 4 };
} // namespace

AxiLiteBridge::AxiLiteBridge(const std::string &name) : SequentialComponent(name, "axi_lite_bridge") {
    addInput("s_axi_awaddr", 32);
    addInput("s_axi_awvalid", 1);
    addInput("s_axi_wdata", 32);
    addInput("s_axi_wstrb", 4);
    addInput("s_axi_wvalid", 1);
    addInput("s_axi_bready", 1);
    addInput("s_axi_araddr", 32);
    addInput("s_axi_arvalid", 1);
    addInput("s_axi_rready", 1);
    addInput("wb_dat_i", 32);
    addInput("wb_ack_i", 1);
    addOutput("s_axi_awready", 1, false, true);
    addOutput("s_axi_wready", 1, false, true);
    addOutput("s_axi_bresp", 2, false, true);
    addOutput("s_axi_bvalid", 1, false, true);
    addOutput("s_axi_arready", 1, false, true);
    addOutput("s_axi_rdata", 32, false, true);
    addOutput("s_axi_rresp", 2, false, true);
    addOutput("s_axi_rvalid", 1, false, true);
    addOutput("wb_adr_o", 32, false, true);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_we_o", 1, false, true);
    addOutput("wb_sel_o", 4, false, true);
    addOutput("wb_stb_o", 1, false, true);
    addOutput("wb_cyc_o", 1, false, true);
}

AxiLiteBridge::~AxiLiteBridge() = default;

bool AxiLiteBridge::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::string AxiLiteBridge::genStateDecl() const {
    return std::format(
            "static uint32_t _axib_aw_addr_{0}, _axib_w_data_{0}, _axib_r_data_{0};\n"
            "static uint32_t _axib_wb_adr_{0}, _axib_wb_dat_{0};\n"
            "static uint8_t _axib_w_strb_{0}, _axib_bresp_{0}, _axib_rresp_{0};\n"
            "static uint8_t _axib_aw_got_{0}, _axib_w_got_{0}, _axib_wb_we_{0}, _axib_wb_stb_{0}, _axib_wb_cyc_{0};\n"
            "static uint8_t _axib_bvalid_{0}, _axib_rvalid_{0}, _axib_st_{0};\n",
            _id);
}

std::string AxiLiteBridge::genInitCode() const {
    return std::format(
            "    _axib_aw_addr_{0} = 0; _axib_w_data_{0} = 0; _axib_r_data_{0} = 0;\n"
            "    _axib_wb_adr_{0} = 0; _axib_wb_dat_{0} = 0;\n"
            "    _axib_w_strb_{0} = 0; _axib_bresp_{0} = 0; _axib_rresp_{0} = 0;\n"
            "    _axib_aw_got_{0} = 0; _axib_w_got_{0} = 0;\n"
            "    _axib_wb_we_{0} = 0; _axib_wb_stb_{0} = 0; _axib_wb_cyc_{0} = 0;\n"
            "    _axib_bvalid_{0} = 0; _axib_rvalid_{0} = 0; _axib_st_{0} = 0;\n",
            _id);
}

std::string AxiLiteBridge::genFuncDef_seq() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    std::string c;
    c += std::format("// AxiLiteBridge {}\n", name());
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    c += "    uint32_t _awaddr = 0; dcs_memcpy(&_awaddr, _w[" + nid(I_AWADDR) + "], 4);\n";
    c += "    uint32_t _wdata = 0; dcs_memcpy(&_wdata, _w[" + nid(I_WDATA) + "], 4);\n";
    c += "    uint32_t _araddr = 0; dcs_memcpy(&_araddr, _w[" + nid(I_ARADDR) + "], 4);\n";
    c += "    uint32_t _wb_dati = 0; dcs_memcpy(&_wb_dati, _w[" + nid(I_WB_DAT) + "], 4);\n";
    c += "    uint8_t _awvalid = _w[" + nid(I_AWVALID) + "][0] & 1;\n";
    c += "    uint8_t _wstrb = _w[" + nid(I_WSTRB) + "][0] & 0x0F;\n";
    c += "    uint8_t _wvalid = _w[" + nid(I_WVALID) + "][0] & 1;\n";
    c += "    uint8_t _bready = _w[" + nid(I_BREADY) + "][0] & 1;\n";
    c += "    uint8_t _arvalid = _w[" + nid(I_ARVALID) + "][0] & 1;\n";
    c += "    uint8_t _rready = _w[" + nid(I_RREADY) + "][0] & 1;\n";
    c += "    uint8_t _wb_ack = _w[" + nid(I_WB_ACK) + "][0] & 1;\n";
    c += "    uint8_t _awready = 0, _wready = 0, _arready = 0;\n";
    c += std::format(
            // ---- 接收 AW / W ----
            "    if (!_axib_aw_got_{0} && _awvalid && _axib_st_{0} == 0) {{\n"
            "        _axib_aw_addr_{0} = _awaddr; _axib_aw_got_{0} = 1; _awready = 1;\n"
            "    }}\n"
            "    if (!_axib_w_got_{0} && _wvalid && _axib_st_{0} == 0) {{\n"
            "        _axib_w_data_{0} = _wdata; _axib_w_strb_{0} = _wstrb; _axib_w_got_{0} = 1; _wready = 1;\n"
            "    }}\n"
            "    if (_axib_st_{0} == 0) {{\n"
            "        if (_axib_aw_got_{0} && _axib_w_got_{0}) {{\n"
            "            _axib_wb_adr_{0} = _axib_aw_addr_{0}; _axib_wb_dat_{0} = _axib_w_data_{0};\n"
            "            _axib_wb_we_{0} = 1; _axib_wb_stb_{0} = 1; _axib_wb_cyc_{0} = 1;\n"
            "            _axib_st_{0} = 1; _axib_aw_got_{0} = 0; _axib_w_got_{0} = 0;\n"
            "        }} else if (_arvalid) {{\n"
            "            _axib_wb_adr_{0} = _araddr; _axib_wb_we_{0} = 0; _axib_wb_stb_{0} = 1; _axib_wb_cyc_{0} = 1;\n"
            "            _arready = 1; _axib_st_{0} = 3;\n"
            "        }}\n"
            "    }} else if (_axib_st_{0} == 1) {{\n"
            "        if (_wb_ack) {{\n"
            "            _axib_wb_stb_{0} = 0; _axib_wb_cyc_{0} = 0;\n"
            "            _axib_bresp_{0} = 0; _axib_bvalid_{0} = 1; _axib_st_{0} = 2;\n"
            "        }}\n"
            "    }} else if (_axib_st_{0} == 2) {{\n"
            "        if (_bready) {{ _axib_bvalid_{0} = 0; _axib_st_{0} = 0; }}\n"
            "    }} else if (_axib_st_{0} == 3) {{\n"
            "        if (_wb_ack) {{\n"
            "            _axib_r_data_{0} = _wb_dati; _axib_wb_stb_{0} = 0; _axib_wb_cyc_{0} = 0;\n"
            "            _axib_rresp_{0} = 0; _axib_rvalid_{0} = 1; _axib_st_{0} = 4;\n"
            "        }}\n"
            "    }} else if (_axib_st_{0} == 4) {{\n"
            "        if (_rready) {{ _axib_rvalid_{0} = 0; _axib_st_{0} = 0; }}\n"
            "    }}\n"
            // 写事务必须按 AXI 的字节使能来；读事务整字有效
            "    const uint8_t _wbsel = _axib_wb_we_{0} ? _axib_w_strb_{0} : 0x0F;\n",
            _id);
    c += "    " + genOutputWrite(O_AWREADY, "_awready", 1) + "\n";
    c += "    " + genOutputWrite(O_WREADY, "_wready", 1) + "\n";
    c += "    " + genOutputWrite(O_BRESP, "_axib_bresp_" + std::to_string(_id), 2) + "\n";
    c += "    " + genOutputWrite(O_BVALID, "_axib_bvalid_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_ARREADY, "_arready", 1) + "\n";
    c += "    " + genOutputWrite(O_RDATA, "_axib_r_data_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_RRESP, "_axib_rresp_" + std::to_string(_id), 2) + "\n";
    c += "    " + genOutputWrite(O_RVALID, "_axib_rvalid_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_WB_ADR, "_axib_wb_adr_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_WB_DAT, "_axib_wb_dat_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_WB_WE, "_axib_wb_we_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_WB_SEL, "_wbsel", 4) + "\n";
    c += "    " + genOutputWrite(O_WB_STB, "_axib_wb_stb_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_WB_CYC, "_axib_wb_cyc_" + std::to_string(_id), 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> AxiLiteBridge::clone(const std::string &new_name) const {
    return std::make_unique<AxiLiteBridge>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"axi_lite_bridge", {}, {"s_axi_awaddr", "s_axi_awvalid", "s_axi_wdata", "s_axi_wstrb", "s_axi_wvalid", "s_axi_bready", "s_axi_araddr", "s_axi_arvalid", "s_axi_rready", "wb_dat_i", "wb_ack_i"}, {"s_axi_awready", "s_axi_wready", "s_axi_bresp", "s_axi_bvalid", "s_axi_arready", "s_axi_rdata", "s_axi_rresp", "s_axi_rvalid", "wb_adr_o", "wb_dat_o", "wb_we_o", "wb_sel_o", "wb_stb_o", "wb_cyc_o"}, dsc::ComponentCategory::Bus, "AXI4-Lite 从设备到 Wishbone 主设备的桥"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::AxiLiteBridge>(n);
        });
} // namespace
