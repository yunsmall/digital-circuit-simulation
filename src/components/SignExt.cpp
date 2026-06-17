// SignExt — 符号扩展元件实现
#include "dcs/components/SignExt.h"
#include <format>
#include <stdexcept>

namespace dsc {

SignExt::SignExt(const std::string &name, int src_width, int dst_width) :
    CombinationalComponent(name, "signext"), _src(src_width), _dst(dst_width) {
    setParam("src_width", std::to_string(src_width));
    setParam("dst_width", std::to_string(dst_width));
    if (src_width < 1 || src_width > 63)
        throw std::invalid_argument("源位宽必须在1-63之间");
    if (dst_width <= src_width || dst_width > 64)
        throw std::invalid_argument("目标位宽必须大于源位宽且不超过64");
    addInput("in", src_width);
    addOutput("out", dst_width);
}

std::string SignExt::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int o_nid = outputs()[0]->netId();

    // 符号扩展: 低 src 位取源值，高位全部填符号位
    // 高位掩码 = dst_mask & ~src_mask（即超出 src 宽度的那些位）
    uint64_t dst_m = (_dst == 64) ? ~0ULL : (1ULL << _dst) - 1;
    uint64_t src_m = (_src == 64) ? ~0ULL : (1ULL << _src) - 1;
    uint64_t hi_m = dst_m & ~src_m;
    std::string hi_hex = (_dst == 64 && _src <= 32) ? std::format("0x{:X}ULL", hi_m) : std::format("0x{:X}ULL", hi_m);

    std::string code;
    code += "static void " + funcName_comb() + "(void) {\n";
    code += "    " + gen_read_wire(in_nid, _src, in_nw, "_in") + "\n";
    code += "    " + std::string(c_int_type(_dst)) + " _out = 0;\n";
    code += "    if (_in & (1ULL << " + std::to_string(_src - 1) + ")) {\n";
    code += "        _out = _in | " + hi_hex + ";\n";
    code += "    } else {\n";
    code += "        _out = _in;\n";
    code += "    }\n";
    code += "    _out &= " + gen_mask(_dst) + ";\n";
    if (o_nid >= 0) {
        code += "    " + genOutputWrite(0, "_out", _dst) + "\n";
        code += "    _c_oe_" + std::to_string(_id) + "_0 = true;\n";
    }
    code += "}\n";
    return code;
}

std::unique_ptr<Component> SignExt::clone(const std::string &n) const {
    return std::make_unique<SignExt>(n, _src, _dst);
}

} // namespace dsc
