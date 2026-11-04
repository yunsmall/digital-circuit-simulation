#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Pwm.h"

#include <format>

namespace dsc {

namespace {
constexpr int I_CLK = 0, I_ADDR = 1, I_WDAT = 2, I_WE = 3, I_EN = 4;
constexpr int O_RDAT = 0, O_READY = 1, O_PWM = 2;
} // namespace

Pwm::Pwm(const std::string &name) : SequentialComponent(name, "pwm") {
    setParam("bit_width", "32");
    addInput("clk", 1);
    addInput("addr", 8);
    addInput("wdata", 32);
    addInput("we", 1);
    addInput("en", 1);
    addOutput("rdata", 32, false, true);
    addOutput("ready", 1, false, true);
    addOutput("pwm", 1, false, true);
}

Pwm::~Pwm() = default;

std::string Pwm::genStateDecl() const {
    return std::format(
            "#define _PWM_{0}_PERIOD 0x00u\n"
            "#define _PWM_{0}_DUTY 0x04u\n"
            "#define _PWM_{0}_CTRL 0x08u\n"
            "static uint32_t _pwm_period_{0}, _pwm_duty_{0}, _pwm_ctrl_{0}, _pwm_cnt_{0};\n"
            "static uint8_t _pwm_prev_en_{0};\n",
            _id);
}

std::string Pwm::genInitCode() const {
    return std::format(
            "    _pwm_period_{0} = 0; _pwm_duty_{0} = 0; _pwm_ctrl_{0} = 0; _pwm_cnt_{0} = 0;\n"
            "    _pwm_prev_en_{0} = 0;\n",
            _id);
}

std::string Pwm::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// PWM {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _addr = 0; dcs_memcpy(&_addr, _w[" + nid(I_ADDR) + "], 1);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_WDAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _en = _w[" + nid(I_EN) + "][0] & 1;\n";
    c += "    uint32_t _rd = 0; uint8_t _ready = 0;\n";
    c += std::format(
            "    const bool _wr = _en && _we && !_pwm_prev_en_{0};\n"
            "    _pwm_prev_en_{0} = _en;\n"
            "    if (_wr) {{\n"
            "        switch (_addr) {{\n"
            "        case _PWM_{0}_PERIOD: _pwm_period_{0} = _wdat; break;\n"
            "        case _PWM_{0}_DUTY: _pwm_duty_{0} = _wdat; break;\n"
            "        case _PWM_{0}_CTRL: _pwm_ctrl_{0} = _wdat & 1u; break;\n"
            "        default: break;\n"
            "        }}\n"
            "    }}\n"
            "    switch (_addr) {{\n"
            "    case _PWM_{0}_PERIOD: _rd = _pwm_period_{0}; break;\n"
            "    case _PWM_{0}_DUTY: _rd = _pwm_duty_{0}; break;\n"
            "    case _PWM_{0}_CTRL: _rd = _pwm_ctrl_{0}; break;\n"
            "    default: _rd = 0; break;\n"
            "    }}\n"
            "    _ready = _en;\n"
            "    uint8_t _p = 0;\n"
            "    if (_pwm_ctrl_{0} && _pwm_period_{0}) {{\n"
            "        if (++_pwm_cnt_{0} >= _pwm_period_{0}) _pwm_cnt_{0} = 0;\n"
            "        _p = (_pwm_cnt_{0} < _pwm_duty_{0}) ? 1 : 0;\n"
            "    }} else _pwm_cnt_{0} = 0;\n",
            _id);
    c += "    " + genOutputWrite(O_RDAT, "_rd", 32) + "\n";
    c += "    " + genOutputWrite(O_READY, "_ready", 1) + "\n";
    c += "    " + genOutputWrite(O_PWM, "_p", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Pwm::clone(const std::string &new_name) const {
    return std::make_unique<Pwm>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"pwm", {}, {"clk", "addr", "wdata", "we", "en"}, {"rdata", "ready", "pwm"}, dsc::ComponentCategory::Misc, "单通道 PWM：PERIOD/DUTY/CTRL 寄存器，输出方波引脚"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::Pwm>(n);
        });
} // namespace
