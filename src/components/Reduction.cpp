// Reduction — 归约门实现
#include "dcs/components/Reduction.h"
#include <format>

namespace dsc {

static const char *red_op_name(ReductionOp op) {
    switch (op) {
        case ReductionOp::AND: return "redand";
        case ReductionOp::OR:  return "redor";
        case ReductionOp::XOR: return "redxor";
    }
    return "redand";
}

static const char *red_op_c(ReductionOp op) {
    switch (op) {
        case ReductionOp::AND: return "&";
        case ReductionOp::OR:  return "|";
        case ReductionOp::XOR: return "^";
    }
    return "&";
}

Reduction::Reduction(const std::string &name, int bit_width, ReductionOp op) :
    CombinationalComponent(name, red_op_name(op)), _bit_width(bit_width), _op(op) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("op", red_op_name(op));
    addInput("in", bit_width);
    addOutput("out", 1);
}

std::string Reduction::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;

    return std::format(R"(static void {}() {{
    {}
    uint8_t _out = _tmp_in & 1;
    for (int _i = 1; _i < {}; _i++)
        _out = _out {} ((_tmp_in >> _i) & 1);
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bit_width, in_nw, "_tmp_in"), _bit_width, red_op_c(_op),
                       genOutputWrite(0, "_out", 1));
}

std::unique_ptr<Component> Reduction::clone(const std::string &n) const {
    return std::make_unique<Reduction>(n, _bit_width, _op);
}

} // namespace dsc
