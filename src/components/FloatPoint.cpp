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
// FloatBinOp
// ============================================================
static const char *float_binop_name(FloatBinOpKind op) {
    switch (op) {
        case FloatBinOpKind::ADD: return "fadd";
        case FloatBinOpKind::SUB: return "fsub";
        case FloatBinOpKind::MUL: return "fmul";
        case FloatBinOpKind::DIV: return "fdiv";
    }
    return "fadd";
}

static const char *float_binop_expr(FloatBinOpKind op) {
    switch (op) {
        case FloatBinOpKind::ADD: return "_a + _b";
        case FloatBinOpKind::SUB: return "_a - _b";
        case FloatBinOpKind::MUL: return "_a * _b";
        case FloatBinOpKind::DIV: return "_a / _b";
    }
    return "_a + _b";
}

FloatBinOp::FloatBinOp(const std::string &name, int precision, FloatBinOpKind op) :
    CombinationalComponent(name, float_binop_name(op)), _precision(precision), _op(op) {
    setParam("precision", std::to_string(precision));
    addInput("a", precision);
    addInput("b", precision);
    addOutput("out", precision);
}

std::string FloatBinOp::genFuncDef_comb() const {
    return gen_float_func(this, _precision, float_binop_expr(_op));
}
std::unique_ptr<Component> FloatBinOp::clone(const std::string &n) const {
    return std::make_unique<FloatBinOp>(n, _precision, _op);
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
    CombinationalComponent(name, std::string("f") + float_cmp_str(op)), _precision(precision), _op(op) {
    setParam("precision", std::to_string(precision));
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

// ============================================================
// FloatToInt — 浮点 → 整型
// ============================================================
FloatToInt::FloatToInt(const std::string &name, int fp, int int_width, bool s) :
    CombinationalComponent(name, "f2i"), _fp(fp), _int_w(int_width), _signed(s) {
    setParam("fp", std::to_string(fp));
    setParam("int_width", std::to_string(int_width));
    setParam("signed", std::to_string(s));
    addInput("in", fp);
    addOutput("out", int_width);
}

std::string FloatToInt::genFuncDef_comb() const {
    auto [ct, bytes] = float_info(_fp);
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;

    // TCC 不支持 __fixunssfdi（float→uint64），绕路 int64_t 避免触发
    // （int64_t→uint64_t 是纯整数转型，不需要编译器内置函数）
    std::string init = "uint64_t _ival = (uint64_t)(int64_t)_fval;";

    return std::format(R"(static void {}() {{
    {}
    {} _fval;
    dcs_memcpy(&_fval, &_tmp_in, {});
    {}
    uint64_t _out = _ival{};
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _fp, in_nw, "_tmp_in"), ct, bytes,
                       init,
                       _int_w < 64 ? std::format(" & {}", gen_mask(_int_w)) : "",
                       genOutputWrite(0, "_out", _int_w));
}

std::unique_ptr<Component> FloatToInt::clone(const std::string &n) const {
    return std::make_unique<FloatToInt>(n, _fp, _int_w, _signed);
}

// ============================================================
// IntToFloat — 整型 → 浮点
// ============================================================
IntToFloat::IntToFloat(const std::string &name, int int_width, int fp, bool s) :
    CombinationalComponent(name, "i2f"), _int_w(int_width), _fp(fp), _signed(s) {
    setParam("int_width", std::to_string(int_width));
    setParam("fp", std::to_string(fp));
    setParam("signed", std::to_string(s));
    addInput("in", int_width);
    addOutput("out", fp);
}

std::string IntToFloat::genFuncDef_comb() const {
    auto [ct, bytes] = float_info(_fp);
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;

    // 符号扩展需要在 uint64_t 上做，_tmp_in 是窄类型会被截断
    std::string conv;
    if (_signed && _int_w < 64) {
        conv = std::format(R"(
    uint64_t _u = _tmp_in;
    if (_u & (1ULL << {}))
        _u |= ~{};
    {} _fout = ({})(int64_t)_u;)", _int_w - 1, gen_mask(_int_w), ct, ct);
    } else if (_signed) {
        // 64 位有符号，直接强转
        conv = std::format(R"(
    {} _fout = ({})(int64_t)_tmp_in;)", ct, ct);
    } else {
        // TCC 不支持 __floatundisf（uint64→float），先转 int64_t
        conv = std::format(R"(
    {} _fout = ({})(int64_t)_tmp_in;)", ct, ct);
    }

    return std::format(R"(static void {}() {{
    {}
    {}
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _int_w, in_nw, "_tmp_in"), conv,
                       genOutputWrite(0, "_fout", _fp));
}

std::unique_ptr<Component> IntToFloat::clone(const std::string &n) const {
    return std::make_unique<IntToFloat>(n, _int_w, _fp, _signed);
}

} // namespace dsc
