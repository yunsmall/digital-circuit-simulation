#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Bcd2Seg.h"

#include <format>

namespace dsc {

namespace {
// a,b,c,d,e,f,g 对应 bit0..bit6；0x3F 就是显示 "0"
const char *kSegTable = "{0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F,0x77,0x7C,0x39,0x5E,0x79,0x71}";
} // namespace

Bcd2Seg::Bcd2Seg(const std::string &name, bool hex_mode, bool active_low) :
    CombinationalComponent(name, "bcd2seg"), _hex_mode(hex_mode), _active_low(active_low) {
    setParam("hex_mode", hex_mode ? "1" : "0");
    setParam("active_low", active_low ? "1" : "0");
    addInput("in", 4);
    addInput("dp", 1);
    addOutput("seg", 8);
}

std::string Bcd2Seg::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int dp_nid = inputs()[1]->netId();
    int dp_nw = dp_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;

    std::string c;
    c += std::format("static const uint8_t _seg_tab_{0}[16] = {1};\n", _id, kSegTable);
    c += std::format("DCS_INLINE void {}() {{\n", funcName_comb());
    c += "    " + gen_read_wire(in_nid, 4, in_nw, "_in") + "\n";
    c += "    " + gen_read_wire(dp_nid, 1, dp_nw, "_dp") + "\n";
    c += std::format("    const uint32_t _v = (uint32_t) _in & 15u;\n");
    c += std::format("    uint8_t _s = _seg_tab_{}[_v];\n", _id);
    if (!_hex_mode)
        c += "    if (_v > 9u) _s = 0; // 非 BCD 码不点亮任何段\n";
    if (_active_low)
        c += "    _s = (uint8_t) ~_s;\n";
    // 小数点单独处理：共阳时也要跟着取反，否则会和段码的极性打架
    c += _active_low ? "    const uint8_t _dpb = _dp ? 0x00 : 0x80;\n" : "    const uint8_t _dpb = _dp ? 0x80 : 0x00;\n";
    c += "    uint8_t _out = (uint8_t) ((_s & 0x7F) | _dpb);\n";
    c += "    " + genOutputWrite(0, "_out", 8) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Bcd2Seg::clone(const std::string &n) const {
    return std::make_unique<Bcd2Seg>(n, _hex_mode, _active_low);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"bcd2seg", {{"hex_mode", "0", {"0", "1"}}, {"active_low", "0", {"0", "1"}}}, {"in", "dp"}, {"seg"},
     dsc::ComponentCategory::Misc, "BCD/十六进制到七段码译码，seg 的 bit0..6 为 a..g，bit7 为小数点"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Bcd2Seg>(n, dsc::paramInt(p, "hex_mode", 0) != 0,
                                                                 dsc::paramInt(p, "active_low", 0) != 0);
                       });
} // namespace
