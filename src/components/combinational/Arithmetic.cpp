#include "dcs/ComponentFactory.h"
#include "dcs/components/combinational/Arithmetic.h"
#include <format>
#include <stdexcept>

namespace dsc {

// 64 位有符号运算（比较/乘/除/取最大最小）在生成的 C 里用 (int64_t)uint64_t
// 做重解释。C 标准把它归为实现定义，这里依赖"二进制补码"这一所有目标平台
// （x86-64/ARM64）都成立的前提；换整数表示的平台才会出问题

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

std::string Mux::genFuncDef_comb() const {
    const char *ct = c_int_type(_bit_width);
    int n = 1 << _n_selects;
    int out_nid = outputs()[0]->netId();

    // 读取选择线
    std::string sel_reads;
    for (int i = 0; i < _n_selects; i++) {
        int nid = inputs()[i]->netId();
        sel_reads += nid >= 0 ? std::format("    bool _sel{} = false; dcs_memcpy(&_sel{}, _w[{}], 1);\n", i, i, nid)
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

    std::string write;
    if (out_nid >= 0)
        write = std::format("    {}\n", genOutputWrite(0, "_out", _bit_width));

    return std::format(R"(static void {}(void) {{
{}
{}
    {} _out = 0;
{}
{}
}})",
                       funcName_comb(), sel_reads, data_reads, ct, select, write);
}

// ============================================================
// Adder
// ============================================================

Adder::Adder(const std::string &name, int bit_width, bool is_signed) :
    CombinationalComponent(name, "adder"), _bit_width(bit_width), _signed(is_signed) {
    setParam("bit_width", std::to_string(bit_width));
    // 有符号模式必须导出：JSON 往返后 cout 的语义（溢出 vs 进位）依赖它
    setParam("signed", is_signed ? "1" : "0");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addInput("cin", 1);
    addOutput("sum", bit_width);
    addOutput("cout", 1);
}

std::string Adder::genFuncDef_comb() const {
    const char *ct = c_int_type(_bit_width);

    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int cin_nid = inputs()[2]->netId();
    int sum_nid = outputs()[0]->netId();
    int cout_nid = outputs()[1]->netId();

    std::string cin_read = cin_nid >= 0 ? std::format("bool _cin = false; dcs_memcpy(&_cin, _w[{}], 1);", cin_nid)
                                        : "bool _cin = false;";

    std::string mask = gen_mask(_bit_width);

    std::string cout_write;
    if (cout_nid >= 0)
        cout_write = std::format("    uint8_t _cout8 = _cout ? 1 : 0;\n"
                                 "    {}\n",
                                 genOutputWrite(1, "_cout8", 1));

    // 有符号模式下 cout 报的是溢出（同号相加得到异号），而不是无符号进位
    // 无符号报最高位进位，有符号报溢出（同号相加得到异号）
    std::string cout_expr;
    if (_signed) {
        int msb = _bit_width - 1;
        cout_expr = std::format("    bool _cout = ((_a >> {}) & 1) == ((_b >> {}) & 1) && ((_sum >> {}) & 1) != ((_a >> {}) & 1);",
                                msb, msb, msb, msb);
    } else if (_bit_width < 64) {
        cout_expr = std::format("    bool _cout = (_full >> {}) != 0;", _bit_width);
    } else {
        cout_expr = "    bool _cout = (_full < _a) || (_cin && _full == _a);";
    }

    // 进位检测：用 uint64_t 扩展精度
    if (_bit_width < 64) {
        return std::format(R"(static void {0}(void) {{
    {1}
    {2}
    {3}
    uint64_t _full = (uint64_t)_a + (uint64_t)_b + (_cin ? 1ULL : 0ULL);
    {4} _sum = ({4})(_full & {5});
    {6}
    {7}
    {8}
}})",
                           funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cin_read, c_int_type(_bit_width), mask,
                           cout_expr, genOutputWrite(0, "_sum", _bit_width), cout_write);
    }
    else {
        // 64 位：不能使用 >> 64（UB），用溢出检测
        return std::format(R"(static void {0}(void) {{
    {1}
    {2}
    {3}
    uint64_t _full = (uint64_t)_a + (uint64_t)_b + (_cin ? 1ULL : 0ULL);
    uint64_t _sum = _full;
    {4}
    {5}
    {6}
}})",
                           funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cin_read, cout_expr,
                           genOutputWrite(0, "_sum", _bit_width), cout_write);
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
    // typeName 必须和工厂注册键逐字一致（"eq"/"ne"...），否则 JSON 回读找不到类型
    CombinationalComponent(name, cmpOpStr(op)), _bit_width(bit_width), _op(op),
    _signed(is_signed) {
    setParam("bit_width", std::to_string(bit_width));
    if (is_signed)
        setParam("signed", "1");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("out", 1);
}

std::string Comparator::genFuncDef_comb() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int out_nid = outputs()[0]->netId();

    // 有符号比较必须先把窄类型符号扩展：_a/_b 是无符号类型，
    // 直接转 int64_t 是零扩展，-1 会变成 255
    std::string cmp_expr;
    if (_signed && _bit_width < 64) {
        std::string mask = gen_mask(_bit_width);
        cmp_expr = std::format(
                "    uint64_t _ua = _a, _ub = _b;\n"
                "    if (_ua & (1ULL << {})) _ua |= ~{};\n"
                "    if (_ub & (1ULL << {})) _ub |= ~{};\n"
                "    uint8_t _out = ((int64_t)_ua {} (int64_t)_ub) ? 1 : 0;",
                _bit_width - 1, mask, _bit_width - 1, mask, cmpOpCOp(_op));
    } else {
        const char *cast = _signed ? "int64_t" : "uint64_t";
        cmp_expr = std::format("    uint8_t _out = (({})({})_a {} ({})({})_b) ? 1 : 0;", cast,
                               c_int_type(_bit_width), cmpOpCOp(_op), cast, c_int_type(_bit_width));
    }
    return std::format(R"(static void {}() {{
    {}
    {}
    {}
    {}
}})",
                       funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                       gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cmp_expr,
                       genOutputWrite(0, "_out", 1));
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

std::string Decoder::genFuncDef_comb() const {
    int n = 1 << _n_selects;

    // 读取选择线，并构建索引
    std::string reads;
    std::string idx = "0";
    for (int i = _n_selects - 1; i >= 0; i--) {
        int nid = inputs()[i]->netId();
        reads += nid >= 0 ? std::format("    bool _sel{} = false; dcs_memcpy(&_sel{}, _w[{}], 1);\n", i, i, nid)
                          : std::format("    bool _sel{} = false;\n", i);
        idx = std::format("(({}) << 1) | _sel{}", idx, i);
    }

    // 输出 one-hot
    std::string writes;
    for (int i = 0; i < n; i++) {
        int out_nid = outputs()[i]->netId();
        if (out_nid >= 0) {
            writes += std::format("    uint8_t _out{} = (_idx == {}) ? 1 : 0;\n", i, i);
            writes += std::format("    {}\n", genOutputWrite(i, std::format("_out{}", i), 1));
        }
    }

    return std::format(R"(static void {}(void) {{
{}
    int _idx = {};
{}
}})",
                       funcName_comb(), reads, idx, writes);
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

std::string Encoder::genFuncDef_comb() const {
    int n = 1 << _n_selects;

    // 读所有输入
    std::string reads;
    for (int i = 0; i < n; i++) {
        int nid = inputs()[i]->netId();
        reads += nid >= 0 ? std::format("    bool _in{} = false; dcs_memcpy(&_in{}, _w[{}], 1);\n", i, i, nid)
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
        writes += std::format("    {}\n", genOutputWrite(i, std::format("_out{}", i), 1));
    }

    return std::format(R"(static void {}() {{
{}
    int _idx = 0;
{}
{}
}})",
                       funcName_comb(), reads, chain, writes);
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

std::string Subtractor::genFuncDef_comb() const {
    const char *ct = c_int_type(_bit_width);
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int bin_nid = inputs()[2]->netId();
    int diff_nid = outputs()[0]->netId();
    int bout_nid = outputs()[1]->netId();

    std::string bin_read = bin_nid >= 0 ? std::format("bool _bin = false; dcs_memcpy(&_bin, _w[{}], 1);", bin_nid)
                                        : "bool _bin = false;";

    std::string bout_write;
    if (bout_nid >= 0)
        bout_write = std::format("    uint8_t _bout8 = _bout ? 1 : 0;\n"
                                 "    {}\n",
                                 genOutputWrite(1, "_bout8", 1));

    // 64 位用溢出检测，<64 位用扩展精度，ct 同时用于类型声明和转型
    if (_bit_width < 64) {
        return std::format(R"(static void {}() {{
    {}
    {}
    {}
    uint64_t _full = (uint64_t)_a - (uint64_t)_b - (_bin ? 1ULL : 0ULL);
    {} _diff = ({})_full & {};
    bool _bout = (_full >> {}) != 0;
    {}
    {}
}})",
                           funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), bin_read, ct, ct, gen_mask(_bit_width),
                           _bit_width, genOutputWrite(0, "_diff", _bit_width), bout_write);
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
                           funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"), bin_read,
                           genOutputWrite(0, "_diff", _bit_width), bout_write);
    }
}

std::unique_ptr<Component> Subtractor::clone(const std::string &n) const {
    return std::make_unique<Subtractor>(n, _bit_width);
}

// ============================================================
// Multiplier — prod = a * b（有符号/无符号）
// ============================================================

Multiplier::Multiplier(const std::string &name, int bit_width, bool is_signed) :
    // typeName 必须是工厂注册键：写成 smul 会让导出的 JSON 回读时找不到类型
    CombinationalComponent(name, "mul"), _bit_width(bit_width), _signed(is_signed) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    if (is_signed)
        setParam("signed", "1");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("prod_lo", bit_width);
    addOutput("prod_hi", bit_width);
}

std::string Multiplier::genFuncDef_comb() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;

    // 纯标准 C：64×64→128 无符号乘法，拆 4 个 32×32→64 部分积
    // 有符号：取绝对值做无符号乘法，再按符号位对 128 位结果取反加一
    // 128 位乘积 [ _hi : _lo ] 中提取 prod_lo[bw:0] 和 prod_hi[2*bw-1:bw]
    std::string mask = gen_mask(_bit_width);

    // 公共：乘法本体 → _lo, _hi（128 位）
    // 只做 128 位乘法，_ua/_ub 由调用方准备：
    // 无符号版是零扩展的原值，有符号版是取过绝对值的，不能在这里重新定义
    auto mul_body = R"(
    uint64_t _ah = _ua >> 32, _al = _ua & 0xFFFFFFFFULL;
    uint64_t _bh = _ub >> 32, _bl = _ub & 0xFFFFFFFFULL;
    uint64_t _p0 = _al * _bl, _p1 = _ah * _bl, _p2 = _al * _bh, _p3 = _ah * _bh;
    uint64_t _lo = _p0;
    _lo += (_p1 & 0xFFFFFFFFULL) << 32; uint64_t _cy = _lo < _p0;
    uint64_t _pv = _lo;
    _lo += (_p2 & 0xFFFFFFFFULL) << 32; _cy += _lo < _pv;
    uint64_t _hi = _p3 + (_p1 >> 32) + (_p2 >> 32) + _cy;)";

    // 从 128 位乘积中提取 hi / lo
    std::string extract_lo, extract_hi;
    if (_bit_width == 64) {
        extract_lo = std::format("{} _prod_lo = ({})_lo;", c_int_type(64), c_int_type(64));
        extract_hi = std::format("{} _prod_hi = ({})_hi;", c_int_type(64), c_int_type(64));
    }
    else {
        extract_lo = std::format("{} _prod_lo = ({})_lo & {};", c_int_type(_bit_width), c_int_type(_bit_width), mask);
        extract_hi = std::format("{} _prod_hi = ({})(((_hi << {}) | (_lo >> {})) & {});", c_int_type(_bit_width),
                                 c_int_type(_bit_width), 64 - _bit_width, _bit_width, mask);
    }

    if (!_signed) {
        return std::format("static void {0}() {{{{\n"
                           "    {1}\n"
                           "    {2}\n"
                           "    {3}\n"
                           "    {4}\n"
                           "    {5}\n"
                           "    {6}\n"
                           "}}}}",
                           funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                           gen_read_wire(b_nid, _bit_width, b_nw, "_b"),
                           "    uint64_t _ua = (uint64_t)_a, _ub = (uint64_t)_b;\n" + std::string(mul_body),
                           extract_lo, extract_hi,
                           genOutputWrite(0, "_prod_lo", _bit_width) + "\n    " +
                                   genOutputWrite(1, "_prod_hi", _bit_width));
    }

    // 有符号版：_a/_b 是无符号类型，直接转 int64_t 是零扩展，
    // 必须先按位宽做符号扩展，否则负数会被当成大正数，符号位判断和绝对值全错
    std::string sext = _bit_width < 64
                               ? std::format("    {{ uint64_t _u = _a; if (_u & (1ULL << {})) _u |= ~{}; _sa = (int64_t)_u; }}\n"
                                             "    {{ uint64_t _u = _b; if (_u & (1ULL << {})) _u |= ~{}; _sb = (int64_t)_u; }}\n",
                                             _bit_width - 1, gen_mask(_bit_width), _bit_width - 1, gen_mask(_bit_width))
                               : std::string("    _sa = (int64_t)_a; _sb = (int64_t)_b;\n");
    return std::format("static void {0}() {{{{\n"
                       "    {1}\n"
                       "    {2}\n"
                       "    int64_t _sa, _sb;\n"
                       "{7}\n"
                       "    int _sign = ((_sa < 0) != (_sb < 0));\n"
                       "    uint64_t _ua = _sa < 0 ? (uint64_t)(-(_sa + 1)) + 1 : (uint64_t)_sa;\n"
                       "    uint64_t _ub = _sb < 0 ? (uint64_t)(-(_sb + 1)) + 1 : (uint64_t)_sb;\n"
                       "{3}\n"
                       "    if (_sign) {{ _lo = ~_lo + 1; _hi = ~_hi + (_lo == 0); }}\n"
                       "    {4}\n"
                       "    {5}\n"
                       "    {6}\n"
                       "}}}}",
                       funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                       gen_read_wire(b_nid, _bit_width, b_nw, "_b"), mul_body, extract_lo, extract_hi,
                       genOutputWrite(0, "_prod_lo", _bit_width) + "\n    " +
                               genOutputWrite(1, "_prod_hi", _bit_width),
                       sext);
}

std::unique_ptr<Component> Multiplier::clone(const std::string &n) const {
    return std::make_unique<Multiplier>(n, _bit_width, _signed);
}

// ============================================================
// Divider — {quot, rem} = a / b（无符号）
// ============================================================

Divider::Divider(const std::string &name, int bit_width, bool is_signed) :
    // 同 Multiplier：typeName 固定用注册键，有符号性只走 signed 参数
    CombinationalComponent(name, "div"), _bit_width(bit_width), _signed(is_signed) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("除法器位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    if (is_signed)
        setParam("signed", "1");
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("quot", bit_width);
    addOutput("rem", bit_width);
}

std::string Divider::genFuncDef_comb() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int q_nid = outputs()[0]->netId();
    int r_nid = outputs()[1]->netId();

    const char *cast = _signed ? "int64_t" : "uint64_t";
    // 有符号除法同样要先做符号扩展，否则 -128/1 会变成 128/1。
    // 用 _ua/_ub 承接（无符号时就是零扩展的原值），后面统一从它算。
    std::string prep = _signed && _bit_width < 64
                               ? std::format("    uint64_t _ua = _a, _ub = _b;\n"
                                             "    if (_ua & (1ULL << {})) _ua |= ~{};\n"
                                             "    if (_ub & (1ULL << {})) _ub |= ~{};\n",
                                             _bit_width - 1, gen_mask(_bit_width), _bit_width - 1, gen_mask(_bit_width))
                               : std::string("    uint64_t _ua = _a, _ub = _b;\n");
    // 负数结果是补码，高位全是 1。引脚位宽不是 8 的倍数时 genOutputWrite 按整字节
    // 写出，不把这些位清掉就会跑进线网，下游同宽引脚又不会做掩码（net_w == pin_w）
    std::string mask_fix;
    if (_signed && _bit_width < 64)
        mask_fix = std::format("    _quot &= (int64_t){};\n    _rem &= (int64_t){};",
                               gen_mask(_bit_width), gen_mask(_bit_width));
    // 全部用编号占位符，避免 std::format 混用限制
    return std::format(R"(static void {0}() {{
    {1}
    {2}
    {6}
    {3} _quot = 0, _rem = 0;
    if (({3})_ub != 0) {{
        // 先按无符号取绝对值再定符号，避开 INT64_MIN / -1 的 UB
        uint64_t _neg = ({3})_ua < 0 ? 1 : 0, _neb = ({3})_ub < 0 ? 1 : 0;
        uint64_t _qa = _neg ? (uint64_t)(-(({3})_ua + 1)) + 1 : ({3})_ua;
        uint64_t _qb = _neb ? (uint64_t)(-(({3})_ub + 1)) + 1 : ({3})_ub;
        uint64_t _q = _qa / _qb, _r = _qa % _qb;
        _quot = (_neg != _neb) ? ({3})(-_q) : ({3})_q;
        _rem  = _neg ? ({3})(-_r) : ({3})_r;
    }}
{7}
    {4}
    {5}
}})",
                       funcName_comb(), gen_read_wire(a_nid, _bit_width, a_nw, "_a"),
                       gen_read_wire(b_nid, _bit_width, b_nw, "_b"), cast, genOutputWrite(0, "_quot", _bit_width),
                       genOutputWrite(1, "_rem", _bit_width), prep, mask_fix);
}

std::unique_ptr<Component> Divider::clone(const std::string &n) const {
    return std::make_unique<Divider>(n, _bit_width, _signed);
}

// ============================================================
// Abs — 绝对值
// ============================================================
Abs::Abs(const std::string &name, int bit_width) :
    CombinationalComponent(name, "abs"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("in", bit_width);
    addOutput("out", bit_width);
}

std::string Abs::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int bw = _bit_width;

    // 纯无符号位运算取反，避免有符号溢出 UB
    return std::format(R"(static void {}() {{
    {}
    uint64_t _v = _tmp_in;
    if (_v & (1ULL << {}))
        _v = (~_v + 1) & {};
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, bw, in_nw, "_tmp_in"), bw - 1, gen_mask(bw),
                       genOutputWrite(0, "_v", bw));
}

std::unique_ptr<Component> Abs::clone(const std::string &n) const {
    return std::make_unique<Abs>(n, _bit_width);
}

// ============================================================
// MinMax — 最小/最大值
// ============================================================
MinMax::MinMax(const std::string &name, int bit_width, bool is_max, bool is_signed) :
    CombinationalComponent(name, is_max ? "max" : "min"),
    _bit_width(bit_width), _is_max(is_max), _signed(is_signed) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    // 不导出 mode：min/max 是两个类型（工厂各自硬编码 is_max），
    // 再导出一个 mode 只会在和 type 冲突时被静默忽略
    setParam("signed", std::to_string(is_signed));
    addInput("a", bit_width);
    addInput("b", bit_width);
    addOutput("out", bit_width);
}

std::string MinMax::genFuncDef_comb() const {
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int b_nid = inputs()[1]->netId();
    int b_nw = b_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int bw = _bit_width;

    std::string cmp;
    if (_signed && bw < 64) {
        // 符号扩展到 int64_t 再比较
        std::string mask = gen_mask(bw);
        cmp = std::format(R"(
    int64_t _sa, _sb;
    {{ uint64_t _u = _a; if (_u & (1ULL << {})) _u |= ~{}; _sa = (int64_t)_u; }}
    {{ uint64_t _u = _b; if (_u & (1ULL << {})) _u |= ~{}; _sb = (int64_t)_u; }}
    uint64_t _out = (_sa {} _sb) ? _a : _b;)", bw - 1, mask, bw - 1, mask, _is_max ? ">" : "<");
    } else if (_signed) {
        // 64 位直接转 int64_t 比较
        cmp = std::format(R"(
    uint64_t _out = ((int64_t)_a {} (int64_t)_b) ? _a : _b;)", _is_max ? ">" : "<");
    } else {
        cmp = std::format(R"(
    uint64_t _out = (_a {} _b) ? _a : _b;)", _is_max ? ">" : "<");
    }

    return std::format(R"(static void {}() {{
    {}
    {}
    {}
    {}
}})",
                       funcName_comb(), gen_read_wire(a_nid, bw, a_nw, "_a"),
                       gen_read_wire(b_nid, bw, b_nw, "_b"), cmp, genOutputWrite(0, "_out", bw));
}

std::unique_ptr<Component> MinMax::clone(const std::string &n) const {
    return std::make_unique<MinMax>(n, _bit_width, _is_max, _signed);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"abs", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Arithmetic, "绝对值，输出=|输入|"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Abs>(n, dsc::paramInt(p, "bit_width", 8));
                       });

DCS_REGISTER_COMPONENT(
    {"min",
                        {{"bit_width", "8"}, {"signed", "0"}},
                        {"a", "b"},
                        {"out"},
                        dsc::ComponentCategory::Arithmetic, "最小值，输出=min(a,b)"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::MinMax>(n, dsc::paramInt(p, "bit_width", 8), false,
                                                                dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"max",
                        {{"bit_width", "8"}, {"signed", "0"}},
                        {"a", "b"},
                        {"out"},
                        dsc::ComponentCategory::Arithmetic, "最大值，输出=max(a,b)"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::MinMax>(n, dsc::paramInt(p, "bit_width", 8), true,
                                                                dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"mux", {{"n_selects", "1"}, {"bit_width", "8"}}, {}, {"out"}, dsc::ComponentCategory::DataPath, "多路选择器，根据sel选择一路输入输出"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Mux>(n, dsc::paramInt(p, "n_selects", 1), dsc::paramInt(p, "bit_width", 8));
                       });

DCS_REGISTER_COMPONENT(
    {"adder", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b", "cin"}, {"sum", "cout"}, dsc::ComponentCategory::Arithmetic, "全加器，sum=a+b+cin, cout=有符号时表示溢出"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Adder>(n, dsc::paramInt(p, "bit_width", 8),
                                                               dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"eq", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "等于比较，a==b时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Comparator>(n, dsc::paramInt(p, "bit_width", 8), dsc::CmpOp::EQ,
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"ne", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "不等于比较，a!=b时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Comparator>(n, dsc::paramInt(p, "bit_width", 8), dsc::CmpOp::NE,
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"lt", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "小于比较，a<b时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Comparator>(n, dsc::paramInt(p, "bit_width", 8), dsc::CmpOp::LT,
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"gt", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "大于比较，a>b时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Comparator>(n, dsc::paramInt(p, "bit_width", 8), dsc::CmpOp::GT,
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"le", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "小于等于比较，a<=b时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Comparator>(n, dsc::paramInt(p, "bit_width", 8), dsc::CmpOp::LE,
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"ge", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "大于等于比较，a>=b时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Comparator>(n, dsc::paramInt(p, "bit_width", 8), dsc::CmpOp::GE,
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"decoder", {{"n_selects", "2"}}, {}, {}, dsc::ComponentCategory::DataPath, "译码器，n位地址→2^n位独热码"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::Decoder>(n, dsc::paramInt(p, "n_selects", 2));
        });

DCS_REGISTER_COMPONENT(
    {"encoder", {{"n_selects", "2"}}, {}, {}, dsc::ComponentCategory::DataPath, "编码器，2^n位独热码→n位二进制"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::Encoder>(n, dsc::paramInt(p, "n_selects", 2));
        });

DCS_REGISTER_COMPONENT(
    {"sub", {{"bit_width", "8"}}, {"a", "b", "bin"}, {"diff", "bout"}, dsc::ComponentCategory::Arithmetic, "全减器，diff=a-b-bin, bout=借位"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Subtractor>(n, dsc::paramInt(p, "bit_width", 8));
                       });

DCS_REGISTER_COMPONENT(
    {"mul", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"prod_lo", "prod_hi"}, dsc::ComponentCategory::Arithmetic, "乘法器，prod=a*b（双倍位宽输出）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Multiplier>(n, dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::paramInt(p, "signed", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"div", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"quot", "rem"}, dsc::ComponentCategory::Arithmetic, "除法器，quot=a/b, rem=a%b"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Divider>(n, dsc::paramInt(p, "bit_width", 8), dsc::paramInt(p, "signed", 0) != 0);
                       });
} // namespace
