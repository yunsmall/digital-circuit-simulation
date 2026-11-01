#include "dcs/ComponentFactory.h"
#include "dcs/components/combinational/Alu.h"

#include <format>
#include <stdexcept>

namespace dsc {

Alu::Alu(const std::string &name, int bit_width) : CombinationalComponent(name, "alu"), _bit_width(bit_width) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    addInput("a", bit_width);
    addInput("b", bit_width);
    addInput("op", 4);
    addOutput("out", bit_width);
    addOutput("zero", 1);
}

std::string Alu::genFuncDef_comb() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int op_nid = inputs()[2]->netId();
    int op_nw = op_nid >= 0 ? inputs()[2]->net()->bit_width() : 0;

    // 有符号操作数：左移到 64 位顶端再算术右移回来，等于按本元件位宽做符号扩展
    std::string sign_a = _bit_width < 64
                                 ? std::format("((int64_t) ((uint64_t) _a << {}) >> {})", 64 - _bit_width, 64 - _bit_width)
                                 : "((int64_t) _a)";
    std::string sign_b = _bit_width < 64
                                 ? std::format("((int64_t) ((uint64_t) _b << {}) >> {})", 64 - _bit_width, 64 - _bit_width)
                                 : "((int64_t) _b)";

    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_comb());
    c += "    " + gen_read_wire(a_nid, _bit_width, a_nw, "_a") + "\n";
    c += "    " + gen_read_wire(b_nid, _bit_width, b_nw, "_b") + "\n";
    c += "    " + gen_read_wire(op_nid, 4, op_nw, "_op") + "\n";
    c += "    const uint64_t _mask = " + gen_mask(_bit_width) + ";\n";
    c += "    uint64_t _r = 0;\n";
    c += "    switch ((uint32_t) _op & 15u) {\n";
    c += "    case 0: _r = (uint64_t) _a + (uint64_t) _b; break;\n";
    c += "    case 1: _r = (uint64_t) _a - (uint64_t) _b; break;\n";
    c += "    case 2: _r = (uint64_t) _a & (uint64_t) _b; break;\n";
    c += "    case 3: _r = (uint64_t) _a | (uint64_t) _b; break;\n";
    c += "    case 4: _r = (uint64_t) _a ^ (uint64_t) _b; break;\n";
    // 移位量按 64 取模，避免 C 里移位超过类型宽度是未定义行为
    c += "    case 5: _r = (uint64_t) _a << ((uint32_t) _b & 63u); break;\n";
    c += "    case 6: _r = (uint64_t) _a >> ((uint32_t) _b & 63u); break;\n";
    c += std::format("    case 7: _r = (uint64_t) ({} >> ((uint32_t) _b & 63u)); break;\n", sign_a);
    c += std::format("    case 8: _r = (uint64_t) ({} < {}); break;\n", sign_a, sign_b);
    c += "    case 9: _r = (uint64_t) ((uint64_t) _a < (uint64_t) _b); break;\n";
    c += "    case 10: _r = (uint64_t) ((uint64_t) _a == (uint64_t) _b); break;\n";
    c += "    case 11: _r = (uint64_t) ((uint64_t) _a != (uint64_t) _b); break;\n";
    c += "    case 12: _r = (uint64_t) _a * (uint64_t) _b; break;\n";
    // 除零没有硬件行为可继承，这里给全 1 让软件能一眼看出异常
    c += "    case 13: _r = ((uint64_t) _b == 0) ? _mask : (uint64_t) _a / (uint64_t) _b; break;\n";
    c += "    case 14: _r = (uint64_t) _a; break;\n";
    c += "    case 15: _r = ~(uint64_t) _a; break;\n";
    c += "    default: _r = 0; break;\n";
    c += "    }\n";
    c += "    _r &= _mask;\n";
    c += "    " + genOutputWrite(0, "_r", _bit_width) + "\n";
    c += "    uint8_t _zero = (_r == 0) ? 1 : 0;\n";
    c += "    " + genOutputWrite(1, "_zero", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Alu::clone(const std::string &n) const {
    return std::make_unique<Alu>(n, _bit_width);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"alu", {{"bit_width", "8"}}, {"a", "b", "op"}, {"out", "zero"}, dsc::ComponentCategory::Arithmetic,
     "多功能 ALU：op=0加 1减 2与 3或 4异或 5左移 6逻辑右移 7算术右移 8有符号小于 9无符号小于 "
     "10等于 11不等 12乘 13无符号除 14传a 15取反"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Alu>(n, dsc::paramInt(p, "bit_width", 8));
                       });
} // namespace
