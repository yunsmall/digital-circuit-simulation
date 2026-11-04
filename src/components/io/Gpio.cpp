#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Gpio.h"

#include <format>

namespace dsc {

namespace {
constexpr int I_CLK = 0, I_ADDR = 1, I_WDAT = 2, I_WE = 3, I_EN = 4, I_GPIO_IN = 5;
constexpr int O_RDAT = 0, O_READY = 1, O_GPIO_OUT = 2;
} // namespace

Gpio::Gpio(const std::string &name) : SequentialComponent(name, "gpio") {
    setParam("bit_width", "32");
    addInput("clk", 1);
    addInput("addr", 8);
    addInput("wdata", 32);
    addInput("we", 1);
    addInput("en", 1);
    addInput("gpio_in", 32);
    addOutput("rdata", 32, false, true);
    addOutput("ready", 1, false, true);
    addOutput("gpio_out", 32, false, true);
}

Gpio::~Gpio() = default;

std::string Gpio::genStateDecl() const {
    return std::format(
            "#define _GPIO_{0}_DIR 0x00u\n"
            "#define _GPIO_{0}_OUT 0x04u\n"
            "#define _GPIO_{0}_IN 0x08u\n"
            "static uint32_t _gpio_dir_{0}, _gpio_out_{0};\n"
            "static uint8_t _gpio_prev_en_{0};\n",
            _id);
}

std::string Gpio::genInitCode() const {
    return std::format("    _gpio_dir_{0} = 0; _gpio_out_{0} = 0; _gpio_prev_en_{0} = 0;\n", _id);
}

std::string Gpio::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// GPIO {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _addr = 0; dcs_memcpy(&_addr, _w[" + nid(I_ADDR) + "], 1);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_WDAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _en = _w[" + nid(I_EN) + "][0] & 1;\n";
    c += "    uint32_t _gin = 0; dcs_memcpy(&_gin, _w[" + nid(I_GPIO_IN) + "], 4);\n";
    c += "    uint32_t _rd = 0; uint8_t _ready = 0;\n";
    c += std::format(
            "    const bool _wr = _en && _we && !_gpio_prev_en_{0};\n"
            "    _gpio_prev_en_{0} = _en;\n"
            "    if (_wr) {{\n"
            "        switch (_addr) {{\n"
            "        case _GPIO_{0}_DIR: _gpio_dir_{0} = _wdat; break;\n"
            "        case _GPIO_{0}_OUT: _gpio_out_{0} = _wdat; break;\n"
            "        default: break;\n"
            "        }}\n"
            "    }}\n"
            "    switch (_addr) {{\n"
            "    case _GPIO_{0}_DIR: _rd = _gpio_dir_{0}; break;\n"
            "    case _GPIO_{0}_OUT: _rd = _gpio_out_{0}; break;\n"
            "    case _GPIO_{0}_IN: _rd = _gin; break;\n"
            "    default: _rd = 0; break;\n"
            "    }}\n"
            "    _ready = _en;\n"
            "    uint32_t _gout = _gpio_out_{0} & _gpio_dir_{0};\n",
            _id);
    c += "    " + genOutputWrite(O_RDAT, "_rd", 32) + "\n";
    c += "    " + genOutputWrite(O_READY, "_ready", 1) + "\n";
    c += "    " + genOutputWrite(O_GPIO_OUT, "_gout", 32) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Gpio::clone(const std::string &new_name) const {
    return std::make_unique<Gpio>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"gpio", {}, {"clk", "addr", "wdata", "we", "en", "gpio_in"}, {"rdata", "ready", "gpio_out"}, dsc::ComponentCategory::Misc, "32 位 GPIO：DIR/OUT/IN 三个寄存器，gpio_in/gpio_out 为外部引脚"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::Gpio>(n);
        });
} // namespace
