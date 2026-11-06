#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Watchdog.h"

#include <format>

namespace dsc {

namespace {
constexpr int I_CLK = 0, I_ADDR = 1, I_WDAT = 2, I_WE = 3, I_EN = 4;
constexpr int O_RDAT = 0, O_READY = 1, O_IRQ = 2;
} // namespace

Watchdog::Watchdog(const std::string &name) : SequentialComponent(name, "watchdog") {
    setParam("bit_width", "32");
    addInput("clk", 1, PinEdge::Rising);
    addInput("addr", 8);
    addInput("wdata", 32);
    addInput("we", 1);
    addInput("en", 1);
    addOutput("rdata", 32, false, true);
    addOutput("ready", 1, false, true);
    addOutput("irq", 1, false, true);
}

Watchdog::~Watchdog() = default;

std::string Watchdog::genStateDecl() const {
    return std::format(
            "#define _WD_{0}_CTRL 0x00u\n"
            "#define _WD_{0}_TIMEOUT 0x04u\n"
            "#define _WD_{0}_FEED 0x08u\n"
            "#define _WD_{0}_STATUS 0x0Cu\n"
            "static uint32_t _wd_ctrl_{0}, _wd_timeout_{0}, _wd_cnt_{0}, _wd_fired_{0};\n"
            "static uint8_t _wd_prev_en_{0};\n",
            _id);
}

std::string Watchdog::genInitCode() const {
    return std::format(
            "    _wd_ctrl_{0} = 0; _wd_timeout_{0} = 0; _wd_cnt_{0} = 0; _wd_fired_{0} = 0;\n"
            "    _wd_prev_en_{0} = 0;\n",
            _id);
}

std::string Watchdog::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// Watchdog {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    const bool _clk_pos = " + genGetWireEdge(I_CLK, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";
    c += "    uint32_t _addr = 0; dcs_memcpy(&_addr, _w[" + nid(I_ADDR) + "], 1);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_WDAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _en = _w[" + nid(I_EN) + "][0] & 1;\n";
    c += "    uint32_t _rd = 0; uint8_t _ready = 0;\n";
    c += std::format(
            "    const bool _wr = _en && _we && !_wd_prev_en_{0};\n"
            "    _wd_prev_en_{0} = _en;\n"
            "    if (_wr) {{\n"
            "        switch (_addr) {{\n"
            "        case _WD_{0}_CTRL: _wd_ctrl_{0} = _wdat & 1u; if (!_wd_ctrl_{0}) {{ _wd_cnt_{0} = 0; _wd_fired_{0} = 0; }} break;\n"
            "        case _WD_{0}_TIMEOUT: _wd_timeout_{0} = _wdat; break;\n"
            "        case _WD_{0}_FEED: _wd_cnt_{0} = 0; _wd_fired_{0} = 0; break;\n"
            "        default: break;\n"
            "        }}\n"
            "    }}\n"
            "    switch (_addr) {{\n"
            "    case _WD_{0}_CTRL: _rd = _wd_ctrl_{0}; break;\n"
            "    case _WD_{0}_TIMEOUT: _rd = _wd_timeout_{0}; break;\n"
            "    case _WD_{0}_STATUS: _rd = _wd_fired_{0}; break;\n"
            "    default: _rd = 0; break;\n"
            "    }}\n"
            "    _ready = _en;\n"
            "    if (_wd_ctrl_{0} && !_wd_fired_{0} && _wd_timeout_{0}) {{\n"
            "        if (++_wd_cnt_{0} >= _wd_timeout_{0}) _wd_fired_{0} = 1;\n"
            "    }}\n"
            "    uint8_t _irq = _wd_fired_{0} ? 1 : 0;\n",
            _id);
    c += "    " + genOutputWrite(O_RDAT, "_rd", 32) + "\n";
    c += "    " + genOutputWrite(O_READY, "_ready", 1) + "\n";
    c += "    " + genOutputWrite(O_IRQ, "_irq", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Watchdog::clone(const std::string &new_name) const {
    return std::make_unique<Watchdog>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"watchdog", {{"bit_width", "32", {"32"}}}, {"clk", "addr", "wdata", "we", "en"}, {"rdata", "ready", "irq"}, dsc::ComponentCategory::Misc, "看门狗定时器：CTRL/TIMEOUT/FEED/STATUS，超时拉 irq"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::Watchdog>(n);
        });
} // namespace
