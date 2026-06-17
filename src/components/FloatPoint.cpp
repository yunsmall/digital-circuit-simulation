// FloatPoint — IEEE 754 浮点运算元件实现
#include "dcs/components/FloatPoint.h"
#include <format>
#include <stdexcept>

namespace dsc {

// 辅助：C 浮点类型名和位宽
static std::pair<const char *, int> float_info(int precision) {
    if (precision == 32)
        return {"float", 4};
    if (precision == 64)
        return {"double", 8};
    throw std::invalid_argument(std::format("浮点精度必须是 32 或 64，当前 {}", precision));
}

// 辅助：genFuncDef_comb 模版
static std::string gen_float_func(const Component *comp, int precision, const char *expr) {
    auto [ct, bytes] = float_info(precision);
    int a_nid = comp->inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? comp->inputs()[0]->net()->bit_width() : 0;
    int b_nid = comp->inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? comp->inputs()[1]->net()->bit_width() : 0;
    int o_nid = comp->outputs()[0]->netId();

    return std::format(R"(static void {}() {{
    {}
    {}
    {} _a = 0, _b = 0;
    dcs_memcpy(&_a, &_tmp_a, {});
    dcs_memcpy(&_b, &_tmp_b, {});
    {} _out = {};
    {}
}})",
                       comp->funcName_comb(), gen_read_wire(a_nid, precision, a_nw, "_tmp_a"),
                       gen_read_wire(b_nid, precision, b_nw, "_tmp_b"), ct, bytes, bytes, ct, expr,
                       comp->genOutputWrite(0, "_out", precision));
}

// ============================================================
// FloatAdd
// ============================================================
FloatAdd::FloatAdd(const std::string &name, int precision) :
    CombinationalComponent(name, std::format("fadd{}", precision)), _precision(precision) {
    auto [ct, bytes] = float_info(precision);
    setParam("precision", std::to_string(precision));
    addInput("a", precision);
    addInput("b", precision);
    addOutput("out", precision);
}

std::string FloatAdd::genFuncDef_comb() const {
    return gen_float_func(this, _precision, "_a + _b");
}
std::unique_ptr<Component> FloatAdd::clone(const std::string &n) const {
    return std::make_unique<FloatAdd>(n, _precision);
}

// ============================================================
// FloatSub
// ============================================================
FloatSub::FloatSub(const std::string &name, int precision) :
    CombinationalComponent(name, std::format("fsub{}", precision)), _precision(precision) {
    auto [ct, bytes] = float_info(precision);
    setParam("precision", std::to_string(precision));
    addInput("a", precision);
    addInput("b", precision);
    addOutput("out", precision);
}

std::string FloatSub::genFuncDef_comb() const {
    return gen_float_func(this, _precision, "_a - _b");
}
std::unique_ptr<Component> FloatSub::clone(const std::string &n) const {
    return std::make_unique<FloatSub>(n, _precision);
}

// ============================================================
// FloatMul
// ============================================================
FloatMul::FloatMul(const std::string &name, int precision) :
    CombinationalComponent(name, std::format("fmul{}", precision)), _precision(precision) {
    auto [ct, bytes] = float_info(precision);
    setParam("precision", std::to_string(precision));
    addInput("a", precision);
    addInput("b", precision);
    addOutput("out", precision);
}

std::string FloatMul::genFuncDef_comb() const {
    return gen_float_func(this, _precision, "_a * _b");
}
std::unique_ptr<Component> FloatMul::clone(const std::string &n) const {
    return std::make_unique<FloatMul>(n, _precision);
}

// ============================================================
// FloatDiv
// ============================================================
FloatDiv::FloatDiv(const std::string &name, int precision) :
    CombinationalComponent(name, std::format("fdiv{}", precision)), _precision(precision) {
    auto [ct, bytes] = float_info(precision);
    setParam("precision", std::to_string(precision));
    addInput("a", precision);
    addInput("b", precision);
    addOutput("out", precision);
}

std::string FloatDiv::genFuncDef_comb() const {
    return gen_float_func(this, _precision, "_a / _b");
}
std::unique_ptr<Component> FloatDiv::clone(const std::string &n) const {
    return std::make_unique<FloatDiv>(n, _precision);
}

// ============================================================
// FloatCmp
// ============================================================
static const char *float_cmp_str(FloatCmpOp op) {
    switch (op) {
        case FloatCmpOp::EQ:
            return "eq";
        case FloatCmpOp::NE:
            return "ne";
        case FloatCmpOp::LT:
            return "lt";
        case FloatCmpOp::GT:
            return "gt";
        case FloatCmpOp::LE:
            return "le";
        case FloatCmpOp::GE:
            return "ge";
    }
    return "eq";
}

static const char *float_cmp_op(FloatCmpOp op) {
    switch (op) {
        case FloatCmpOp::EQ:
            return "==";
        case FloatCmpOp::NE:
            return "!=";
        case FloatCmpOp::LT:
            return "<";
        case FloatCmpOp::GT:
            return ">";
        case FloatCmpOp::LE:
            return "<=";
        case FloatCmpOp::GE:
            return ">=";
    }
    return "==";
}

FloatCmp::FloatCmp(const std::string &name, int precision, FloatCmpOp op) :
    CombinationalComponent(name, std::format("fcmp{}_{}", precision, float_cmp_str(op))), _precision(precision),
    _op(op) {
    auto [ct, bytes] = float_info(precision);
    setParam("precision", std::to_string(precision));
    setParam("op", float_cmp_str(op));
    addInput("a", precision);
    addInput("b", precision);
    addOutput("out", 1);
}

std::string FloatCmp::genFuncDef_comb() const {
    auto [ct, bytes] = float_info(_precision);
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int o_nid = outputs()[0]->netId();

    return std::format(R"(static void {}() {{
    {}
    {}
    {} _a, _b;
    dcs_memcpy(&_a, &_tmp_a, {});
    dcs_memcpy(&_b, &_tmp_b, {});
    uint8_t _out = (_a {} _b) ? 1 : 0;
    {}
}})",
                       funcName_comb(), gen_read_wire(a_nid, _precision, a_nw, "_tmp_a"),
                       gen_read_wire(b_nid, _precision, b_nw, "_tmp_b"), ct, bytes, bytes, float_cmp_op(_op),
                       genOutputWrite(0, "_out", 1));
}

std::unique_ptr<Component> FloatCmp::clone(const std::string &n) const {
    return std::make_unique<FloatCmp>(n, _precision, _op);
}

// ============================================================
// FloatConst — 浮点常量
// ============================================================
FloatConst::FloatConst(const std::string &name, int precision, double value) :
    CombinationalComponent(name, std::format("fconst{}", precision)), _precision(precision), _value(value) {
    auto [ct, bytes] = float_info(precision);
    setParam("precision", std::to_string(precision));
    setParam("value", std::format("{}", value));
    addOutput("out", precision);
}

std::string FloatConst::genFuncDef_comb() const {
    auto [ct, bytes] = float_info(_precision);
    int o_nid = outputs()[0]->netId();

    return std::format(R"(static void {}() {{
    {} _c = {};
    {}
}})",
                       funcName_comb(), ct, std::format("{:.9g}", _value), genOutputWrite(0, "_c", _precision));
}

std::unique_ptr<Component> FloatConst::clone(const std::string &n) const {
    return std::make_unique<FloatConst>(n, _precision, _value);
}

} // namespace dsc
