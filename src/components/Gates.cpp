#include "dcs/components/Gates.h"
#include <format>
#include <stdexcept>
#include <sstream>

namespace dsc {

// ============================================================
// LogicGate — 多输入逻辑门
// ============================================================
static const char *gate_op_name(GateOp op) {
    switch (op) {
        case GateOp::AND:  return "and";
        case GateOp::OR:   return "or";
        case GateOp::NAND: return "nand";
        case GateOp::NOR:  return "nor";
        case GateOp::XOR:  return "xor";
        case GateOp::XNOR: return "xnor";
    }
    return "and";
}

static const char *gate_op_str(GateOp op) {
    switch (op) {
        case GateOp::AND:  return "&";
        case GateOp::OR:   return "|";
        case GateOp::NAND: return "~(&)";
        case GateOp::NOR:  return "~(|)";
        case GateOp::XOR:  return "^";
        case GateOp::XNOR: return "~(^)";
    }
    return "&";
}

LogicGate::LogicGate(const std::string &name, int num_inputs, int bit_width, GateOp op) :
    CombinationalComponent(name, gate_op_name(op)), _bit_width(bit_width), _num_inputs(num_inputs), _op(op) {
    setParam("inputs", std::to_string(num_inputs));
    setParam("bit_width", std::to_string(bit_width));
    if (num_inputs < 2 || num_inputs > 8)
        throw std::invalid_argument(std::format("LogicGate 输入数量必须在2-8之间，当前{}", num_inputs));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    for (int i = 0; i < num_inputs; i++)
        addInput(std::format("in{}", i), bit_width);
    addOutput("out", bit_width);
}

std::string LogicGate::genFuncDef_comb() const {
    // 读取所有输入
    std::string reads;
    for (int i = 0; i < _num_inputs; i++) {
        int nid = inputs()[i]->netId(), nw = nid >= 0 ? inputs()[i]->net()->bit_width() : 0;
        reads += std::format("    {}\n", gen_read_wire(nid, _bit_width, nw, std::format("_in{}", i)));
    }

    // 生成表达式
    std::ostringstream expr;
    bool neg = (_op == GateOp::NAND || _op == GateOp::NOR || _op == GateOp::XNOR);
    if (neg)
        expr << "~(";
    const char *op_str = (_op == GateOp::AND || _op == GateOp::NAND) ? " & "
                         : (_op == GateOp::OR || _op == GateOp::NOR)  ? " | "
                         :                                                " ^ ";
    for (int i = 0; i < _num_inputs; i++) {
        if (i > 0)
            expr << op_str;
        expr << "_in" << i;
    }
    if (neg)
        expr << ")";

    return std::format(R"(static void {}(void) {{
{}
    {} _out = {};
    {}
}})",
                       funcName_comb(), reads, c_int_type(_bit_width), expr.str(),
                       genOutputWrite(0, "_out", _bit_width));
}

std::unique_ptr<Component> LogicGate::clone(const std::string &n) const {
    return std::make_unique<LogicGate>(n, _num_inputs, _bit_width, _op);
}

// ============================================================
// UnaryGate — 单输入门（NOT / BUF）
// ============================================================
UnaryGate::UnaryGate(const std::string &name, int bit_width, bool invert) :
    CombinationalComponent(name, invert ? "not" : "buf"), _bit_width(bit_width), _invert(invert) {
    setParam("bit_width", std::to_string(bit_width));
    addInput("in", bit_width);
    addOutput("out", bit_width);
}

std::string UnaryGate::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;

    return std::format(R"(static void {}() {{
    {}
    {} _out = {};
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bit_width, in_nw, "_in"), c_int_type(_bit_width),
                       _invert ? std::format("~_in & {}", gen_mask(_bit_width)) : "_in",
                       genOutputWrite(0, "_out", _bit_width));
}

std::unique_ptr<Component> UnaryGate::clone(const std::string &n) const {
    return std::make_unique<UnaryGate>(n, _bit_width, _invert);
}

// ============================================================
// GateTSBUF — 三态缓冲器
// ============================================================
GateTSBUF::GateTSBUF(const std::string &name, int bit_width) :
    CombinationalComponent(name, "tsbuf"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    addInput("in", bit_width);
    addInput("oe", 1);
    addOutput("out", bit_width, true);
}

std::string GateTSBUF::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int oe_nid = inputs()[1]->netId();

    std::string read_oe = oe_nid >= 0 ? std::format("uint8_t _oe = 0; dcs_memcpy(&_oe, _w[{}], 1);", oe_nid)
                                      : "uint8_t _oe = 1;";

    return std::format(R"(static void {}() {{
    {}
    {}
    {}
    _c_oe_{}_0 = _oe;
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bit_width, in_nw, "_in"), read_oe,
                       genOutputWrite(0, "_in", _bit_width), _id);
}

std::unique_ptr<Component> GateTSBUF::clone(const std::string &n) const {
    return std::make_unique<GateTSBUF>(n, _bit_width);
}

} // namespace dsc
