// Misc — 常量、开关、合并、拆分元件实现
#include "dcs/components/Misc.h"
#include <format>
#include <stdexcept>

namespace dsc {

// ============================================================
// Constant — 固定输出常量
// ============================================================

Constant::Constant(const std::string &name, int bit_width, uint64_t value) :
    CombinationalComponent(name, "const"), _bit_width(bit_width), _value(value) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("value", std::to_string(_value));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addOutput("out", bit_width);
}

std::string Constant::genFuncDef_comb() const {
    int o0 = outputs()[0]->netId();
    if (o0 < 0)
        return std::format("static void {}() {{ /* 输出悬空 */ }}", funcName_comb());

    int bytes = byte_count(_bit_width);
    uint64_t mask = (_bit_width == 64) ? ~0ULL : (1ULL << _bit_width) - 1;
    std::string code;
    code += std::format("static void {}() {{\n", funcName_comb());
    code += std::format("    {} _c = 0x{:X}ULL;\n", c_int_type(_bit_width), _value & mask);
    code += std::format("    {}\n", genOutputWrite(0, "_c", _bit_width));
    code += std::format("    _c_oe_{}_0 = true;\n", _id);
    code += "}\n";
    return code;
}

std::unique_ptr<Component> Constant::clone(const std::string &n) const {
    return std::make_unique<Constant>(n, _bit_width, _value);
}

// ============================================================
// Switch — 使能导通
// ============================================================

Switch::Switch(const std::string &name, int bit_width) : CombinationalComponent(name, "switch"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("in", bit_width);
    addInput("en", 1);
    addOutput("out", bit_width);
}

std::string Switch::genFuncDef_comb() const {
    int i0 = inputs()[0]->netId(), i0nw = i0 >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int en = inputs()[1]->netId();
    int o0 = outputs()[0]->netId();
    std::string write;
    if (o0 >= 0)
        write = std::format("    {}\n    _c_oe_{}_0 = true;\n", genOutputWrite(0, "_out", _bit_width), _id);
    return std::format(R"(static void {}() {{
    {}
    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);
    {} _out = _en ? _in : ({})0;
{}
}})",
                       funcName_comb(), gen_read_wire(i0, _bit_width, i0nw, "_in"), en >= 0 ? en : 0,
                       c_int_type(_bit_width), c_int_type(_bit_width), write);
}

std::unique_ptr<Component> Switch::clone(const std::string &n) const {
    return std::make_unique<Switch>(n, _bit_width);
}

// ============================================================
// Merge — 多位 1-bit 合并为总线
// ============================================================

Merge::Merge(const std::string &name, int num_bits) : CombinationalComponent(name, "merge"), _num_bits(num_bits) {
    setParam("num_bits", std::to_string(num_bits));
    if (num_bits < 2 || num_bits > 64)
        throw std::invalid_argument("合并位数必须在2-64之间");
    for (int i = 0; i < num_bits; i++)
        addInput(std::format("in{}", i), 1);
    addOutput("out", num_bits);
}

std::string Merge::genFuncDef_comb() const {
    int out_nid = outputs()[0]->netId();
    int bytes = byte_count(_num_bits);

    // 读每位输入，移位拼接
    std::string reads;
    for (int i = 0; i < _num_bits; i++) {
        int nid = inputs()[i]->netId();
        reads += "    " +
                 gen_read_wire(nid, 1, nid >= 0 ? inputs()[i]->net()->bit_width() : 0, std::format("_b{}", i)) + "\n";
    }

    std::string code;
    code += std::format("static void {}() {{\n", funcName_comb());
    code += reads;
    code += std::format("    {} _out = 0;\n", c_int_type(_num_bits));
    for (int i = 0; i < _num_bits; i++)
        code += std::format("    _out |= (({})_b{}) << {};\n", c_int_type(_num_bits), i, i);
    code += std::format("    {}\n", genOutputWrite(0, "_out", _num_bits));
    code += std::format("    _c_oe_{}_0 = true;\n", _id);
    code += "}\n";
    return code;
}

std::unique_ptr<Component> Merge::clone(const std::string &n) const {
    return std::make_unique<Merge>(n, _num_bits);
}

// ============================================================
// Split — 总线拆分为多位 1-bit
// ============================================================

Split::Split(const std::string &name, int num_bits) : CombinationalComponent(name, "split"), _num_bits(num_bits) {
    setParam("num_bits", std::to_string(num_bits));
    if (num_bits < 2 || num_bits > 64)
        throw std::invalid_argument("拆分位数必须在2-64之间");
    addInput("in", num_bits);
    for (int i = 0; i < num_bits; i++)
        addOutput(std::format("out{}", i), 1);
}

std::string Split::genFuncDef_comb() const {
    int i0 = inputs()[0]->netId(), i0nw = i0 >= 0 ? inputs()[0]->net()->bit_width() : 0;

    std::string code;
    code += std::format("static void {}() {{\n", funcName_comb());
    code += "    " + gen_read_wire(i0, _num_bits, i0nw, "_in") + "\n";
    for (int i = 0; i < _num_bits; i++) {
        int nid = outputs()[i]->netId();
        if (nid >= 0) {
            code += std::format("    uint8_t _b{} = (_in >> {}) & 1;\n", i, i);
            code += std::format("    {}\n", genOutputWrite(i, std::format("_b{}", i), 1));
            code += std::format("    _c_oe_{}_{} = true;\n", _id, i);
        }
    }
    code += "}\n";
    return code;
}

std::unique_ptr<Component> Split::clone(const std::string &n) const {
    return std::make_unique<Split>(n, _num_bits);
}

} // namespace dsc
