#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/RegBridge.h"

#include <format>

namespace dsc {

namespace {
constexpr int I_CLK = 0, I_ADR = 1, I_DAT = 2, I_WE = 3, I_SEL = 4, I_STB = 5, I_CYC = 6;
constexpr int I_RDAT = 7, I_READY = 8;
constexpr int O_DAT = 0, O_ACK = 1, O_ADDR = 2, O_WDAT = 3, O_WE = 4, O_EN = 5;
} // namespace

RegBridge::RegBridge(const std::string &name) : SequentialComponent(name, "reg_bridge") {
    addInput("clk", 1);
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("rdata", 32);
    addInput("ready", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("addr", 8, false, true);
    addOutput("wdata", 32, false, true);
    addOutput("we", 1, false, true);
    addOutput("en", 1, false, true);
}

RegBridge::~RegBridge() = default;

std::string RegBridge::genStateDecl() const {
    return std::format(
            "static uint8_t _rb_busy_{0}, _rb_ack_{0};\n"
            "static uint32_t _rb_latched_{0};\n"
            // 事务期间 addr/wdata/we 必须保持稳定：元件可能要到几拍之后才给 ready
            "static uint32_t _rb_laddr_{0}, _rb_ldat_{0};\n"
            "static uint8_t _rb_lwe_{0};\n",
            _id);
}

std::string RegBridge::genInitCode() const {
    return std::format(
            "    _rb_busy_{0} = 0; _rb_ack_{0} = 0; _rb_latched_{0} = 0;\n"
            "    _rb_laddr_{0} = 0; _rb_ldat_{0} = 0; _rb_lwe_{0} = 0;\n",
            _id);
}

std::string RegBridge::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// RegBridge {}\n", name());
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(I_ADR) + "], 4);\n";
    c += "    uint32_t _dat = 0; dcs_memcpy(&_dat, _w[" + nid(I_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _stb = _w[" + nid(I_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(I_CYC) + "][0] & 1;\n";
    c += "    uint32_t _rdat = 0; dcs_memcpy(&_rdat, _w[" + nid(I_RDAT) + "], 4);\n";
    c += "    uint8_t _ready = _w[" + nid(I_READY) + "][0] & 1;\n";
    c += std::format(
            "    const bool _active = _stb && _cyc;\n"
            "    uint8_t _en = 0;\n"
            "    if (!_rb_busy_{0}) {{\n"
            "        if (_active) {{ _en = 1; _rb_laddr_{0} = _adr & 0xFF; _rb_ldat_{0} = _dat; _rb_lwe_{0} = _we; _rb_busy_{0} = 1; _rb_ack_{0} = 0; }}\n"
            "    }} else {{\n"
            "        if (_ready) {{ _rb_latched_{0} = _rdat; _rb_ack_{0} = 1; }}\n"
            "        if (!_active) {{ _rb_busy_{0} = 0; _rb_ack_{0} = 0; }}\n"
            "    }}\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_rb_latched_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_rb_ack_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_ADDR, "_rb_laddr_" + std::to_string(_id), 8) + "\n";
    c += "    " + genOutputWrite(O_WDAT, "_rb_ldat_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_WE, "_rb_lwe_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_EN, "_en", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> RegBridge::clone(const std::string &new_name) const {
    return std::make_unique<RegBridge>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"reg_bridge", {}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "rdata", "ready"}, {"wb_dat_o", "wb_ack_o", "addr", "wdata", "we", "en"}, dsc::ComponentCategory::Misc, "Wishbone 从设备转通用寄存器接口（addr/wdata/we/en → rdata/ready）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::RegBridge>(n);
        });
} // namespace
