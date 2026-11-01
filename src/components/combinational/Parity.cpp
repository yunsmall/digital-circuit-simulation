#include "dcs/ComponentFactory.h"
#include "dcs/components/combinational/Parity.h"

#include <format>
#include <stdexcept>

namespace dsc {

Parity::Parity(const std::string &name, int bit_width, bool mode_odd) :
    CombinationalComponent(name, "parity"), _bit_width(bit_width), _mode_odd(mode_odd) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("mode", mode_odd ? "odd" : "even");
    addInput("in", bit_width);
    addOutput("out", 1);
}

std::string Parity::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;

    std::string c;
    c += std::format("static void {}() {{\n", funcName_comb());
    c += "    " + gen_read_wire(in_nid, _bit_width, in_nw, "_in") + "\n";
    // 折半按位异或算奇偶，不依赖 __builtin_parity，TCC 也照常生成直白的移位异或
    c += "    uint64_t _x = (uint64_t) _in;\n";
    c += "    _x ^= _x >> 32; _x ^= _x >> 16; _x ^= _x >> 8;\n";
    c += "    _x ^= _x >> 4; _x ^= _x >> 2; _x ^= _x >> 1;\n";
    c += _mode_odd ? "    uint8_t _out = (uint8_t) ((_x & 1u) ^ 1u);\n" : "    uint8_t _out = (uint8_t) (_x & 1u);\n";
    c += "    " + genOutputWrite(0, "_out", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Parity::clone(const std::string &n) const {
    return std::make_unique<Parity>(n, _bit_width, _mode_odd);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"parity", {{"bit_width", "8"}, {"mode", "even", {"even", "odd"}}}, {"in"}, {"out"},
     dsc::ComponentCategory::Arithmetic, "奇偶校验位生成：even 让数据+校验位共偶数个 1，odd 相反"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Parity>(n, dsc::paramInt(p, "bit_width", 8),
                                                                dsc::paramStr(p, "mode", "even") == "odd");
                       });
} // namespace
