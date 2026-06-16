#include "dcs/components/Arithmetic.h"
#include <format>
#include <stdexcept>

namespace dsc {

// ============================================================
// Mux
// ============================================================

Mux::Mux(const std::string &name, int n_selects, int bit_width) :
    CombinationalComponent(name, "mux"), _n_selects(n_selects), _bit_width(bit_width) {
    setParam("n_selects", std::to_string(n_selects));
    setParam("bit_width", std::to_string(bit_width));
    if (n_selects < 1 || n_selects > 4)
        throw std::invalid_argument(std::format("Mux选择线数量必须在1-4之间，当前{}", n_selects));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");

    for (int i = 0; i < n_selects; i++)
        addInput(std::format("sel{}", i), 1);

    int n = 1 << n_selects;
    for (int i = 0; i < n; i++)
        addInput(std::format("in{}", i), bit_width);

    addOutput("out", bit_width);
}

std::string Mux::genFuncDef() const {
    const char *ct = c_int_type(_bit_width);
    int bytes = byte_count(_bit_width);
    int n = 1 << _n_selects;
    int out_nid = outputs()[0]->netId();

    // 读取选择线
    std::string sel_reads;
    for (int i = 0; i < _n_selects; i++) {
        int nid = inputs()[i]->netId();
        sel_reads += nid >= 0 ? std::format("    bool _sel{}; dcs_memcpy(&_sel{}, _w[{}], 1);\n", i, i, nid)
                              : std::format("    bool _sel{} = false;\n", i);
    }

    // 读取数据输入
    std::string data_reads;
    for (int i = 0; i < n; i++) {
        int nid = inputs()[_n_selects + i]->netId();
        int nw = nid >= 0 ? inputs()[_n_selects + i]->net()->bit_width() : 0;
        data_reads += std::format("    {}\n", gen_read_wire(nid, _bit_width, nw, std::format("_in{}", i)));
    }

    // 构建选择表达式：根据 sel 位组合出索引
    std::string sel_idx;
    if (_n_selects == 1)
        sel_idx = "_sel0";
    else {
        sel_idx = "(";
        for (int i = 0; i < _n_selects; i++) {
            if (i > 0)
                sel_idx += " | (";
            sel_idx += std::format("_sel{} << {}", i, i);
            if (i > 0)
                sel_idx += ")";
        }
        sel_idx += ")";
    }

    // if-else 链选择输出
    std::string select;
    for (int i = 0; i < n; i++) {
        select += std::format("    {}if ({} == {}) _out = _in{};\n", i == 0 ? "" : "else ", sel_idx, i, i);
    }

    std::string write = out_nid >= 0 ? std::format("    {}\n", gen_write_wire(out_nid, "_out", _bit_width)) : "";

    return std::format(R"(static void {}(void) {{
{}
{}
    {} _out = 0;
{}
{}
}})",
                       funcName(), sel_reads, data_reads, ct, select, write);
}

// ============================================================
// Adder
// ============================================================

Adder::Adder(const std::string &name, int bit_width, bool is_signed) : CombinationalComponent(name, "adder"), _bit_width(bit_width), _signed(is_signed) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addInput("cin", 1);
    addOutput("sum", bit_width);
    addOutput("cout", 1);
}

std::string Adder::genFuncDef() const {
    const char *ct = c_int_type(_bit_width);
    int bytes = byte_count(_bit_width);

    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int cin_nid = inputs()[2]->netId();
    int sum_nid = outputs()[0]->netId();
    int cout_nid = outputs()[1]->netId();

    std::string cin_read =
            cin_nid >= 0 ? std::format("bool _cin; dcs_memcpy(&_cin, _w[{}], 1);", cin_nid) : "bool _cin = false;";

    std::string mask = gen_mask(_bit_width);

    std::string cout_write = cout_nid >= 0 ? std::format("    uint8_t _cout8 = _cout ? 1 : 0;\n"
                                                         "    dcs_memcpy(_w[{}], &_cout8, 1);"
                                                         "    dcs_memset(_w[{}] + 1, 0, 15);\n",
                                                         cout_nid, cout_nid)
                                           : "";

    // 进位检测：用 uint64_t 扩展精度
    if (_bit_width < 64) {
        return std::format(R"(static void {0}(void) {{
    {1}
    {2}
    {3}
    uint64_t _full = (uint64_t)_a + (uint64_t)_b + (_cin ? 1ULL : 0ULL);
    {4} _sum = ({4})(_full & {5});
    bool _cout = (_full >> {6}) != 0;
    {7}
    {8}
}})",
                           funcName(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cin_read, c_int_type(_bit_width), mask,
                           _bit_width, gen_write_wire(sum_nid, "_sum", _bit_width), cout_write);
    }
    else {
        // 64 位：不能使用 >> 64（UB），用溢出检测
        return std::format(R"(static void {0}(void) {{
    {1}
    {2}
    {3}
    uint64_t _full = (uint64_t)_a + (uint64_t)_b + (_cin ? 1ULL : 0ULL);
    uint64_t _sum = _full;
    bool _cout = (_full < _a) || (_cin && _full == _a);
    {4}
    {5}
}})",
                           funcName(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cin_read,
                           gen_write_wire(sum_nid, "_sum", _bit_width), cout_write);
    }
}

// ============================================================
// Comparator
// ============================================================

static const char *cmpOpStr(CmpOp op) {
    switch (op) {
        case CmpOp::EQ:
            return "eq";
        case CmpOp::NE:
            return "ne";
        case CmpOp::LT:
            return "lt";
        case CmpOp::GT:
            return "gt";
        case CmpOp::LE:
            return "le";
        case CmpOp::GE:
            return "ge";
    }
    return "eq";
}

static const char *cmpOpCOp(CmpOp op) {
    switch (op) {
        case CmpOp::EQ:
            return "==";
        case CmpOp::NE:
            return "!=";
        case CmpOp::LT:
            return "<";
        case CmpOp::GT:
            return ">";
        case CmpOp::LE:
            return "<=";
        case CmpOp::GE:
            return ">=";
    }
    return "==";
}

Comparator::Comparator(const std::string &name, int bit_width, CmpOp op, bool is_signed) :
    CombinationalComponent(name, std::string("cmp_") + cmpOpStr(op)), _bit_width(bit_width), _op(op),
    _signed(is_signed) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("op", cmpOpStr(_op));
    if (is_signed)
        setParam("signed", "1");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("out", 1);
}

std::string Comparator::genFuncDef() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int out_nid = outputs()[0]->netId();

    // 有符号：int64_t 转型比较；无符号：uint64_t 转型比较
    const char *cast = _signed ? "int64_t" : "uint64_t";
    return std::format(R"(static void {}() {{
    {}
    {}
    uint8_t _out = (({})({})_a {} ({})({})_b) ? 1 : 0;
    {}
}})",
                       funcName(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                       gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cast, c_int_type(_bit_width), cmpOpCOp(_op), cast,
                       c_int_type(_bit_width), gen_write_wire(out_nid, "_out", 1));
}

// ============================================================
// Decoder — N 位选择 → 2^N 位 one-hot
// ============================================================

Decoder::Decoder(const std::string &name, int n_selects) :
    CombinationalComponent(name, "decoder"), _n_selects(n_selects) {
    setParam("n_selects", std::to_string(n_selects));
    if (n_selects < 1 || n_selects > 8)
        throw std::invalid_argument(std::format("Decoder选择位宽必须在1-8之间，当前{}", n_selects));

    for (int i = 0; i < n_selects; i++)
        addInput(std::format("sel{}", i), 1);

    int n = 1 << n_selects;
    for (int i = 0; i < n; i++)
        addOutput(std::format("out{}", i), 1);
}

std::string Decoder::genFuncDef() const {
    int n = 1 << _n_selects;

    // 读取选择线，并构建索引
    std::string reads;
    std::string idx = "0";
    for (int i = _n_selects - 1; i >= 0; i--) {
        int nid = inputs()[i]->netId();
        reads += nid >= 0 ? std::format("    bool _sel{}; dcs_memcpy(&_sel{}, _w[{}], 1);\n", i, i, nid)
                          : std::format("    bool _sel{} = false;\n", i);
        idx = std::format("(({}) << 1) | _sel{}", idx, i);
    }

    // 输出 one-hot
    std::string writes;
    for (int i = 0; i < n; i++) {
        int out_nid = outputs()[i]->netId();
        if (out_nid >= 0) {
            writes += std::format("    uint8_t _out{} = (_idx == {}) ? 1 : 0;\n", i, i);
            writes += std::format("    {}\n", gen_write_wire(out_nid, std::format("_out{}", i), 1));
        }
    }

    return std::format(R"(static void {}(void) {{
{}
    int _idx = {};
{}
}})",
                       funcName(), reads, idx, writes);
}


// ============================================================
// clone() 实现
// ============================================================

std::unique_ptr<Component> Mux::clone(const std::string &n) const {
    return std::make_unique<Mux>(n, _n_selects, _bit_width);
}
std::unique_ptr<Component> Adder::clone(const std::string &n) const {
    return std::make_unique<Adder>(n, _bit_width, _signed);
}
std::unique_ptr<Component> Comparator::clone(const std::string &n) const {
    return std::make_unique<Comparator>(n, _bit_width, _op, _signed);
}
std::unique_ptr<Component> Decoder::clone(const std::string &n) const {
    return std::make_unique<Decoder>(n, _n_selects);
}

// ============================================================
// Encoder — 2^N 位 one-hot → N 位二进制
// ============================================================

Encoder::Encoder(const std::string &name, int n_selects) :
    CombinationalComponent(name, "encoder"), _n_selects(n_selects) {
    if (n_selects < 1 || n_selects > 8)
        throw std::invalid_argument(std::format("Encoder选择位宽必须在1-8之间，当前{}", n_selects));
    setParam("n_selects", std::to_string(n_selects));

    int n = 1 << n_selects;
    for (int i = 0; i < n; i++)
        addInput(std::format("in{}", i), 1);
    for (int i = 0; i < n_selects; i++)
        addOutput(std::format("out{}", i), 1);
}

std::string Encoder::genFuncDef() const {
    int n = 1 << _n_selects;

    // 读所有输入
    std::string reads;
    for (int i = 0; i < n; i++) {
        int nid = inputs()[i]->netId();
        reads += nid >= 0 ? std::format("    bool _in{}; dcs_memcpy(&_in{}, _w[{}], 1);\n", i, i, nid)
                          : std::format("    bool _in{} = false;\n", i);
    }

    // if-else 链：查找第一个为 1 的输入
    std::string chain;
    for (int i = 0; i < n; i++) {
        chain += std::format("    {}if (_in{}) {{ _idx = {}; }}\n", i == 0 ? "" : "else ", i, i);
    }

    // 输出
    std::string writes;
    for (int i = 0; i < _n_selects; i++) {
        int nid = outputs()[i]->netId();
        if (nid < 0)
            continue;
        writes += std::format("    uint8_t _out{} = (_idx >> {}) & 1;\n", i, i);
        writes += std::format("    dcs_memcpy(_w[{}], &_out{}, 1);\n", nid, i);
    }

    return std::format(R"(static void {}() {{
{}
    int _idx = 0;
{}
{}
}})",
                       funcName(), reads, chain, writes);
}

std::unique_ptr<Component> Encoder::clone(const std::string &n) const {
    return std::make_unique<Encoder>(n, _n_selects);
}

// ============================================================
// Subtractor — {bout, diff} = a - b - bin
// ============================================================

Subtractor::Subtractor(const std::string &name, int bit_width) :
    CombinationalComponent(name, "sub"), _bit_width(bit_width) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    addInput("a", bit_width);
    addInput("b", bit_width);
    addInput("bin", 1);
    addOutput("diff", bit_width);
    addOutput("bout", 1);
}

std::string Subtractor::genFuncDef() const {
    const char *ct = c_int_type(_bit_width);
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int bin_nid = inputs()[2]->netId();
    int diff_nid = outputs()[0]->netId();
    int bout_nid = outputs()[1]->netId();

    std::string bin_read =
            bin_nid >= 0 ? std::format("bool _bin; dcs_memcpy(&_bin, _w[{}], 1);", bin_nid) : "bool _bin = false;";

    std::string bout_write = bout_nid >= 0 ? std::format("    uint8_t _bout8 = _bout ? 1 : 0;\n"
                                                         "    dcs_memcpy(_w[{}], &_bout8, 1);"
                                                         "    dcs_memset(_w[{}] + 1, 0, 15);\n",
                                                         bout_nid, bout_nid)
                                           : "";

    // 64 位用溢出检测，<64 位用扩展精度，ct 同时用于类型声明和转型
    if (_bit_width < 64) {
        return std::format(R"(static void {}() {{
    {}
    {}
    {}
    uint64_t _full = (uint64_t)_a - (uint64_t)_b - (_bin ? 1ULL : 0ULL);
    {} _diff = ({})_full;
    bool _bout = (_full >> {}) != 0;
    {}
    {}
}})",
                           funcName(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), bin_read, ct, ct, _bit_width,
                           gen_write_wire(diff_nid, "_diff", _bit_width), bout_write);
    }
    else {
        return std::format(R"(static void {}() {{
    {}
    {}
    {}
    uint64_t _diff = (uint64_t)_a - (uint64_t)_b - (_bin ? 1ULL : 0ULL);
    bool _bout = (_a < _b) || (_bin && _a == _b);
    {}
    {}
}})",
                           funcName(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), bin_read,
                           gen_write_wire(diff_nid, "_diff", _bit_width), bout_write);
    }
}

std::unique_ptr<Component> Subtractor::clone(const std::string &n) const {
    return std::make_unique<Subtractor>(n, _bit_width);
}

// ============================================================
// Multiplier — prod = a * b（有符号/无符号）
// ============================================================

Multiplier::Multiplier(const std::string &name, int bit_width, bool is_signed) :
    CombinationalComponent(name, is_signed ? "smul" : "mul"), _bit_width(bit_width), _signed(is_signed) {
    if (bit_width < 1 || bit_width > 32)
        throw std::invalid_argument("乘法器位宽必须在1-32之间（乘积最大64位）");
    setParam("bit_width", std::to_string(bit_width));
    if (is_signed) setParam("signed", "1");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("prod", bit_width * 2);
}

std::string Multiplier::genFuncDef() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int prod_nid = outputs()[0]->netId();
    int prod_w = _bit_width * 2;

    // 有符号：int64_t 符号扩展相乘；无符号：uint64_t 零扩展相乘
    const char* cast = _signed ? "int64_t" : "uint64_t";
    return std::format(R"(static void {}() {{
    {}
    {}
    {} _prod = ({})_a * ({})_b;
    {}
}})",
        funcName(),
        gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
        gen_read_wire(b_nid, _bit_width, b_nw, "_b"),
        c_int_type(prod_w), cast, cast,
        gen_write_wire(prod_nid, "_prod", prod_w));
}

std::unique_ptr<Component> Multiplier::clone(const std::string &n) const {
    return std::make_unique<Multiplier>(n, _bit_width, _signed);
}

// ============================================================
// Divider — {quot, rem} = a / b（无符号）
// ============================================================

Divider::Divider(const std::string &name, int bit_width, bool is_signed)
    : CombinationalComponent(name, is_signed ? "sdiv" : "div"), _bit_width(bit_width), _signed(is_signed) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("除法器位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    if (is_signed) setParam("signed", "1");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("quot", bit_width);
    addOutput("rem", bit_width);
}

std::string Divider::genFuncDef() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int q_nid = outputs()[0]->netId();
    int r_nid = outputs()[1]->netId();

    const char* cast = _signed ? "int64_t" : "uint64_t";
    // 全部用编号占位符，避免 std::format 混用限制
    return std::format(R"(static void {0}() {{
    {1}
    {2}
    {3} _quot = 0, _rem = 0;
    if (({3})_b != 0) {{
        _quot = ({3})_a / ({3})_b;
        _rem  = ({3})_a % ({3})_b;
    }}
    {4}
    {5}
}})",
        funcName(),
        gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
        gen_read_wire(b_nid, _bit_width, b_nw, "_b"),
        cast,
        gen_write_wire(q_nid, "_quot", _bit_width),
        gen_write_wire(r_nid, "_rem", _bit_width));
}

std::unique_ptr<Component> Divider::clone(const std::string &n) const {
    return std::make_unique<Divider>(n, _bit_width, _signed);
}

} // namespace dsc
