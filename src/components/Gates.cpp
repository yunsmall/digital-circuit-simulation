#include "dcs/components/Gates.h"
#include <format>
#include <stdexcept>

namespace dsc {

// 宏简化 clone 实现
#define DCS_CLONE_GATE(Class)                                                                                          \
    std::unique_ptr<Component> Class::clone(const std::string &n) const {                                              \
        return std::make_unique<Class>(n, numInputs(), bitWidth());                                                    \
    }

// ============================================================
// 多输入门
// ============================================================

GateAND::GateAND(const std::string &name, int num_inputs, int bit_width) :
    MultiInputGate(name, "and", num_inputs, bit_width) {
}
void GateAND::writeExpr(std::ostringstream &oss, int n) {
    for (int i = 0; i < n; i++) {
        if (i > 0)
            oss << " & ";
        oss << "_in" << i;
    }
}
DCS_CLONE_GATE(GateAND)

GateOR::GateOR(const std::string &name, int num_inputs, int bit_width) :
    MultiInputGate(name, "or", num_inputs, bit_width) {
}
void GateOR::writeExpr(std::ostringstream &oss, int n) {
    for (int i = 0; i < n; i++) {
        if (i > 0)
            oss << " | ";
        oss << "_in" << i;
    }
}
DCS_CLONE_GATE(GateOR)

GateNAND::GateNAND(const std::string &name, int num_inputs, int bit_width) :
    MultiInputGate(name, "nand", num_inputs, bit_width) {
}
void GateNAND::writeExpr(std::ostringstream &oss, int n) {
    oss << "~(";
    for (int i = 0; i < n; i++) {
        if (i > 0)
            oss << " & ";
        oss << "_in" << i;
    }
    oss << ")";
}
DCS_CLONE_GATE(GateNAND)

GateNOR::GateNOR(const std::string &name, int num_inputs, int bit_width) :
    MultiInputGate(name, "nor", num_inputs, bit_width) {
}
void GateNOR::writeExpr(std::ostringstream &oss, int n) {
    oss << "~(";
    for (int i = 0; i < n; i++) {
        if (i > 0)
            oss << " | ";
        oss << "_in" << i;
    }
    oss << ")";
}
DCS_CLONE_GATE(GateNOR)

GateXOR::GateXOR(const std::string &name, int num_inputs, int bit_width) :
    MultiInputGate(name, "xor", num_inputs, bit_width) {
}
void GateXOR::writeExpr(std::ostringstream &oss, int n) {
    for (int i = 0; i < n; i++) {
        if (i > 0)
            oss << " ^ ";
        oss << "_in" << i;
    }
}
DCS_CLONE_GATE(GateXOR)

GateXNOR::GateXNOR(const std::string &name, int num_inputs, int bit_width) :
    MultiInputGate(name, "xnor", num_inputs, bit_width) {
}
void GateXNOR::writeExpr(std::ostringstream &oss, int n) {
    oss << "~(";
    for (int i = 0; i < n; i++) {
        if (i > 0)
            oss << " ^ ";
        oss << "_in" << i;
    }
    oss << ")";
}
DCS_CLONE_GATE(GateXNOR)

#undef DCS_CLONE_GATE

// ============================================================
// 单输入门
// ============================================================

GateNOT::GateNOT(const std::string &name, int bit_width) : CombinationalComponent(name, "not"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("in", bit_width);
    addOutput("out", bit_width);
}
std::string GateNOT::genFuncDef_comb() const {
    int i0 = inputs()[0]->netId(), nw = i0 >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int o0 = outputs()[0]->netId();
    std::string write;
    if (o0 >= 0)
        write = std::format("    {}\n    _c_oe_{}_0 = true;\n", genOutputWrite(0, "_out", _bit_width), _id);
    return std::format(R"(static void {}(void) {{
    {}
    {} _out = ~_in;
    _out &= {};
{}
}})",
                       funcName_comb(), gen_read_wire(i0, _bit_width, nw, "_in"), c_int_type(_bit_width),
                       gen_mask(_bit_width), write);
}
std::unique_ptr<Component> GateNOT::clone(const std::string &n) const {
    return std::make_unique<GateNOT>(n, _bit_width);
}

GateBUF::GateBUF(const std::string &name, int bit_width) : CombinationalComponent(name, "buf"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("in", bit_width);
    addOutput("out", bit_width);
}
std::string GateBUF::genFuncDef_comb() const {
    int i0 = inputs()[0]->netId(), nw = i0 >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int o0 = outputs()[0]->netId();
    std::string write;
    if (o0 >= 0)
        write = std::format("    {}\n    _c_oe_{}_0 = true;\n", genOutputWrite(0, "_out", _bit_width), _id);
    return std::format(R"(static void {}(void) {{
    {}
    {} _out = _in;
{}
}})",
                       funcName_comb(), gen_read_wire(i0, _bit_width, nw, "_in"), c_int_type(_bit_width), write);
}
std::unique_ptr<Component> GateBUF::clone(const std::string &n) const {
    return std::make_unique<GateBUF>(n, _bit_width);
}

GateTSBUF::GateTSBUF(const std::string &name, int bit_width) :
    CombinationalComponent(name, "tsbuf"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("in", bit_width);
    addInput("oe", 1);
    addOutput("out", bit_width, true); // 三态输出
}
std::string GateTSBUF::genFuncDef_comb() const {
    int i0 = inputs()[0]->netId(), nw = i0 >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int oe_nid = inputs()[1]->netId(), o0 = outputs()[0]->netId();
    std::string oe_r = oe_nid >= 0 ? std::format("    bool _oe = false; dcs_memcpy(&_oe, _w[{}], 1);", oe_nid)
                                   : "    bool _oe = false;";
    // 始终写候选数据，oe 标志由 oe 引脚决定
    std::string wb;
    if (o0 >= 0)
        wb = std::format("    {}\n    _c_oe_{}_0 = _oe;\n", genOutputWrite(0, "_in", _bit_width), _id);
    return std::format(R"(static void {}(void) {{
    {}
    {}
{}
}})",
                       funcName_comb(), gen_read_wire(i0, _bit_width, nw, "_in"), oe_r, wb);
}
std::unique_ptr<Component> GateTSBUF::clone(const std::string &n) const {
    return std::make_unique<GateTSBUF>(n, _bit_width);
}
} // namespace dsc
