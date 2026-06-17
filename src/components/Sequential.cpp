#include "dcs/components/Sequential.h"
#include <format>
#include <stdexcept>

namespace dsc {

// ============================================================
// D 触发器
// ============================================================
DFlipFlop::DFlipFlop(const std::string &name, int bit_width, bool has_en, bool has_rst, bool has_preset) :
    SequentialComponent(name, "dff"), _bit_width(bit_width), _has_en(has_en), _has_rst(has_rst),
    _has_preset(has_preset) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("has_en", _has_en ? "1" : "0");
    setParam("has_rst", _has_rst ? "1" : "0");
    setParam("has_preset", _has_preset ? "1" : "0");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("d", bit_width);
    addInput("clk", 1);
    if (_has_en)
        addInput("en", 1);
    if (_has_rst)
        addInput("rst", 1);
    if (_has_preset)
        addInput("pre", 1);
    addOutput("q", bit_width, false, true);
}

std::string DFlipFlop::genStructDef() const {
    return std::format(R"(typedef struct {{
    uint8_t q[16];
    uint8_t prev_clk;
}} {};)",
                       stateTypeName());
}
std::string DFlipFlop::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string DFlipFlop::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}

std::string DFlipFlop::genFuncDef_seq() const {
    int d_nid = inputs()[0]->netId();
    int d_nw = d_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int clk_nid = inputs()[1]->netId();
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    int en_nid = _has_en ? inputs()[2]->netId() : -1;
    int rst_nid = _has_rst ? inputs()[_has_en ? 3 : 2]->netId() : -1;
    int pre_nid = _has_preset ? inputs()[_has_en + _has_rst + 1]->netId() : -1;

    std::string read_d = gen_read_wire(d_nid, _bit_width, d_nw, "_dval");
    std::string read_clk = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";

    std::string read_opt;
    if (_has_en && en_nid >= 0)
        read_opt += std::format("\n    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid);
    else if (_has_en)
        read_opt += "\n    bool _en = true;";
    else
        read_opt += "\n    bool _en = true;";
    if (_has_rst && rst_nid >= 0)
        read_opt += std::format("\n    bool _rst = false; dcs_memcpy(&_rst, _w[{}], 1);", rst_nid);
    if (_has_preset && pre_nid >= 0)
        read_opt += std::format("\n    bool _pre = false; dcs_memcpy(&_pre, _w[{}], 1);", pre_nid);

    std::string edge = std::format("bool _rising = (_clk && !{}.prev_clk);", st);

    std::string update;
    if (_has_rst && _has_preset) {
        update = std::format(R"(    if (_rst) {{
        dcs_memset({0}.q, 0, {1});
    }} else if (_pre) {{
        dcs_memset({0}.q, 0xFF, {1});
        {0}.q[{1} - 1] &= {2};
    }} else if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes, gen_mask(_bit_width));
    }
    else if (_has_rst) {
        update = std::format(R"(    if (_rst) {{
        dcs_memset({0}.q, 0, {1});
    }} else if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes);
    }
    else if (_has_preset) {
        update = std::format(R"(    if (_pre) {{
        dcs_memset({0}.q, 0xFF, {1});
        {0}.q[{1} - 1] &= {2};
    }} else if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes, gen_mask(_bit_width));
    }
    else {
        update = std::format(R"(    if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes);
    }

    std::string save_clk = std::format("{}.prev_clk = _clk;", st);
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空";

    return std::format(R"(static void {0}(void) {{
    {1}
    {2}{3}
    {4}
    {5}
    {6}
    {7}
}})",
                       funcName_seq(), read_d, read_clk, read_opt, edge, update, save_clk, write_q);
}

// ============================================================
// T 触发器
// ============================================================
TFlipFlop::TFlipFlop(const std::string &name, int bit_width) : SequentialComponent(name, "tff"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("t", 1);
    addInput("clk", 1);
    addOutput("q", bit_width, false, true);
}
std::string TFlipFlop::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t q[16]; uint8_t prev_clk; }} {};)", stateTypeName());
}
std::string TFlipFlop::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string TFlipFlop::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string TFlipFlop::genFuncDef_seq() const {
    int t_nid = inputs()[0]->netId();
    int clk_nid = inputs()[1]->netId();
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    std::string read_t =
            t_nid >= 0 ? std::format("bool _t = false; dcs_memcpy(&_t, _w[{}], 1);", t_nid) : "bool _t = false;";
    std::string read_clk = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1}
    {2}
    bool _rising = (_clk && !{3}.prev_clk);
    if (_rising && _t) {{
        for (int _i = 0; _i < {4}; _i++) {3}.q[_i] = ~{3}.q[_i];
        {3}.q[{4} - 1] &= {5};
    }}
    {3}.prev_clk = _clk;
{6}
}})",
                       funcName_seq(), read_t, read_clk, st, bytes, gen_mask(_bit_width), write_q);
}

// ============================================================
// JK 触发器
// ============================================================
JKFlipFlop::JKFlipFlop(const std::string &name, int bit_width) :
    SequentialComponent(name, "jkff"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("j", 1);
    addInput("k", 1);
    addInput("clk", 1);
    addOutput("q", bit_width, false, true);
}
std::string JKFlipFlop::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t q[16]; uint8_t prev_clk; }} {};)", stateTypeName());
}
std::string JKFlipFlop::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string JKFlipFlop::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string JKFlipFlop::genFuncDef_seq() const {
    int j_nid = inputs()[0]->netId(), k_nid = inputs()[1]->netId(), clk_nid = inputs()[2]->netId();
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    std::string read_j =
            j_nid >= 0 ? std::format("bool _j = false; dcs_memcpy(&_j, _w[{}], 1);", j_nid) : "bool _j = false;";
    std::string read_k =
            k_nid >= 0 ? std::format("bool _k = false; dcs_memcpy(&_k, _w[{}], 1);", k_nid) : "bool _k = false;";
    std::string read_c = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                      : "bool _clk = false;";
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1} {2} {3}
    bool _rising = (_clk && !{4}.prev_clk);
    if (_rising) {{
        if (_j && _k) {{
            for (int _i = 0; _i < {5}; _i++) {4}.q[_i] = ~{4}.q[_i];
            {4}.q[{5} - 1] &= {6};
        }} else if (_j) {{ dcs_memset({4}.q, 0xFF, {5}); {4}.q[{5} - 1] &= {6};
        }} else if (_k) {{ dcs_memset({4}.q, 0, {5}); }}
    }}
    {4}.prev_clk = _clk;
{7}
}})",
                       funcName_seq(), read_j, read_k, read_c, st, bytes, gen_mask(_bit_width), write_q);
}

// ============================================================
// 寄存器
// ============================================================
Register::Register(const std::string &name, int bit_width, bool has_en, bool has_rst) :
    SequentialComponent(name, "reg"), _bit_width(bit_width), _has_en(has_en), _has_rst(has_rst) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("has_en", _has_en ? "1" : "0");
    setParam("has_rst", _has_rst ? "1" : "0");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("d", bit_width);
    addInput("clk", 1);
    if (_has_en)
        addInput("en", 1);
    if (_has_rst)
        addInput("rst", 1);
    addOutput("q", bit_width, false, true);
}
std::string Register::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t data[16]; uint8_t prev_clk; }} {};)", stateTypeName());
}
std::string Register::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string Register::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string Register::genFuncDef_seq() const {
    int d_nid = inputs()[0]->netId(), clk_nid = inputs()[1]->netId(), q_nid = outputs()[0]->netId();
    int d_nw = d_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    int en_nid = _has_en ? inputs()[2]->netId() : -1;
    int rst_nid = _has_rst ? inputs()[_has_en ? 3 : 2]->netId() : -1;

    std::string read_d = gen_read_wire(d_nid, _bit_width, d_nw, "_dval");
    std::string read_clk = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";
    std::string read_opt;
    if (_has_en && en_nid >= 0)
        read_opt += std::format("\n    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid);
    else if (_has_en)
        read_opt += "\n    bool _en = true;";
    else
        read_opt += "\n    bool _en = true;";
    if (_has_rst && rst_nid >= 0)
        read_opt += std::format("\n    bool _rst = false; dcs_memcpy(&_rst, _w[{}], 1);", rst_nid);

    std::string update;
    if (_has_rst) {
        update = std::format(R"(    if (_rst) {{ dcs_memset({0}.data, 0, {1}); }}
    else if (_rising && _en) {{ dcs_memcpy({0}.data, &_dval, {1}); }})",
                             st, bytes);
    }
    else {
        update = std::format(R"(    if (_rising && _en) {{ dcs_memcpy({0}.data, &_dval, {1}); }})", st, bytes);
    }
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.data", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1} {2}{3}
    bool _rising = (_clk && !{4}.prev_clk);
{5}
    {4}.prev_clk = _clk;
{6}
}})",
                       funcName_seq(), read_d, read_clk, read_opt, st, update, write_q);
}

// ============================================================
// 锁存器
// ============================================================
Latch::Latch(const std::string &name, int bit_width) : SequentialComponent(name, "latch"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("d", bit_width);
    addInput("en", 1);
    addOutput("q", bit_width, false, true);
}
std::string Latch::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t q[16]; }} {};)", stateTypeName());
}
std::string Latch::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string Latch::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string Latch::genFuncDef_seq() const {
    int d_nid = inputs()[0]->netId(), en_nid = inputs()[1]->netId(), q_nid = outputs()[0]->netId();
    int d_nw = d_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    std::string read_d = gen_read_wire(d_nid, _bit_width, d_nw, "_dval");
    std::string read_en =
            en_nid >= 0 ? std::format("bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid) : "bool _en = false;";
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1} {2}
    if (_en) {{ dcs_memcpy({3}.q, &_dval, {4}); }}
{5}
}})",
                       funcName_seq(), read_d, read_en, st, bytes, write_q);
}

// ============================================================
// 计数器
// ============================================================
Counter::Counter(const std::string &name, int bit_width, bool has_load, bool has_en, bool has_updown, bool has_clr) :
    SequentialComponent(name, "cnt"), _bit_width(bit_width), _has_load(has_load), _has_en(has_en),
    _has_updown(has_updown), _has_clr(has_clr) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("has_load", _has_load ? "1" : "0");
    setParam("has_en", _has_en ? "1" : "0");
    setParam("has_updown", _has_updown ? "1" : "0");
    setParam("has_clr", _has_clr ? "1" : "0");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("clk", 1);
    if (_has_load) {
        addInput("load", 1);
        addInput("din", bit_width);
    }
    if (_has_en)
        addInput("en", 1);
    if (_has_updown)
        addInput("updown", 1);
    if (_has_clr)
        addInput("clr", 1);
    addOutput("q", bit_width, false, true);
}
std::string Counter::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t count[16]; uint8_t prev_clk; }} {};)", stateTypeName());
}
std::string Counter::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string Counter::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string Counter::genFuncDef_seq() const {
    int clk_nid = inputs()[0]->netId();
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    int pi = 1;
    int load_nid = _has_load ? inputs()[pi++]->netId() : -1;
    int din_nid = _has_load ? inputs()[pi++]->netId() : -1;
    int en_nid = _has_en ? inputs()[pi++]->netId() : -1;
    int updown_nid = _has_updown ? inputs()[pi++]->netId() : -1;
    int clr_nid = _has_clr ? inputs()[pi++]->netId() : -1;

    std::string read_clk = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";
    std::string read_opt, load_decl;
    if (_has_load) {
        load_decl = load_nid >= 0 ? std::format("bool _load = false; dcs_memcpy(&_load, _w[{}], 1);", load_nid)
                                  : "bool _load = false;";
        int din_nw = din_nid >= 0 ? inputs()[2]->net()->bit_width() : 0;
        read_opt += std::format("\n    {}\n    {}", load_decl, gen_read_wire(din_nid, _bit_width, din_nw, "_din"));
        load_decl = "";
    }
    else {
        load_decl = "bool _load = false;";
    }

    if (_has_en && en_nid >= 0)
        read_opt += std::format("\n    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid);
    else if (_has_en)
        read_opt += "\n    bool _en = true;";
    else
        read_opt += "\n    bool _en = true;";
    if (_has_updown && updown_nid >= 0)
        read_opt += std::format("\n    bool _updown = false; dcs_memcpy(&_updown, _w[{}], 1);", updown_nid);
    if (_has_clr && clr_nid >= 0)
        read_opt += std::format("\n    bool _clr = false; dcs_memcpy(&_clr, _w[{}], 1);", clr_nid);
    else if (_has_clr)
        read_opt += "\n    bool _clr = false;";

    std::string inc_dec;
    if (_has_updown) {
        inc_dec = std::format(R"(        if (_updown) {{
            uint16_t _borrow = 1;
            for (int _i = 0; _i < {0}; _i++) {{ uint16_t _tmp = {1}.count[_i] - _borrow; {1}.count[_i] = (uint8_t)_tmp; _borrow = (_tmp >> 8) & 1; }}
        }} else {{
            uint16_t _carry = 1;
            for (int _i = 0; _i < {0}; _i++) {{ uint16_t _tmp = {1}.count[_i] + _carry; {1}.count[_i] = (uint8_t)_tmp; _carry = (_tmp >> 8) & 1; }}
        }}
        {1}.count[{0} - 1] &= {2};)",
                              bytes, st, gen_mask(_bit_width));
    }
    else {
        inc_dec = std::format(R"(        uint16_t _carry = 1;
        for (int _i = 0; _i < {0}; _i++) {{ uint16_t _tmp = {1}.count[_i] + _carry; {1}.count[_i] = (uint8_t)_tmp; _carry = (_tmp >> 8) & 1; }}
        {1}.count[{0} - 1] &= {2};)",
                              bytes, st, gen_mask(_bit_width));
    }

    std::string body;
    if (_has_load) {
        body = std::format(R"(        if (_load) {{ dcs_memcpy({0}.count, &_din, {1}); }} else {{
{2}
        }})",
                           st, bytes, inc_dec);
    }
    else {
        body = inc_dec;
    }

    std::string update;
    if (_has_clr) {
        update = std::format(R"(    if (_clr) {{ dcs_memset({0}.count, 0, {1}); }}
    else if (_rising && _en) {{
{2}
    }})",
                             st, bytes, body);
    }
    else {
        update = std::format(R"(    if (_rising && _en) {{
{1}
    }})",
                             st, body);
    }

    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.count", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1}{2}
    bool _rising = (_clk && !{3}.prev_clk);
    {4}
{5}
    {3}.prev_clk = _clk;
{6}
}})",
                       funcName_seq(), read_clk, read_opt, st, load_decl, update, write_q);
}

// ============================================================
// 移位寄存器
// ============================================================
ShiftRegister::ShiftRegister(const std::string &name, int length, bool has_dir, bool has_clr) :
    SequentialComponent(name, "sr"), _length(length), _has_dir(has_dir), _has_clr(has_clr) {
    setParam("length", std::to_string(_length));
    setParam("has_dir", _has_dir ? "1" : "0");
    setParam("has_clr", _has_clr ? "1" : "0");
    if (length < 1 || length > 64)
        throw std::invalid_argument("移位寄存器长度必须在1-64之间");
    addInput("sin", 1);
    addInput("clk", 1);
    if (_has_dir)
        addInput("dir", 1);
    if (_has_clr)
        addInput("clr", 1);
    addOutput("q", length, false, true);
}
std::string ShiftRegister::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t stages[16]; uint8_t prev_clk; }} {};)", stateTypeName());
}
std::string ShiftRegister::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string ShiftRegister::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string ShiftRegister::genFuncDef_seq() const {
    int sin_nid = inputs()[0]->netId(), clk_nid = inputs()[1]->netId(), q_nid = outputs()[0]->netId();
    int bytes = byte_count(_length);
    auto st = stateVarName();
    int dir_nid = _has_dir ? inputs()[2]->netId() : -1;
    int clr_nid = _has_clr ? inputs()[_has_dir ? 3 : 2]->netId() : -1;
    int utype = _length == 64 ? 64 : (_length <= 8 ? 8 : _length <= 16 ? 16 : _length <= 32 ? 32 : 64);

    std::string read_sin = sin_nid >= 0 ? std::format("bool _sin = false; dcs_memcpy(&_sin, _w[{}], 1);", sin_nid)
                                        : "bool _sin = false;";
    std::string read_clk = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";
    std::string read_opt;
    if (_has_dir && dir_nid >= 0)
        read_opt += std::format("\n    bool _dir = false; dcs_memcpy(&_dir, _w[{}], 1);", dir_nid);
    if (_has_clr && clr_nid >= 0)
        read_opt += std::format("\n    bool _clr = false; dcs_memcpy(&_clr, _w[{}], 1);", clr_nid);
    else if (_has_clr)
        read_opt += "\n    bool _clr = false;";

    auto make_shift = [&](bool left) -> std::string {
        return std::format(R"(        uint{0}_t _tmp;
        dcs_memcpy(&_tmp, {1}.stages, {2});
        _tmp = ({3}) | (_sin ? {4} : 0);
        _tmp &= {5};
        dcs_memcpy({1}.stages, &_tmp, {2});)",
                           utype, st, bytes, left ? "(_tmp << 1)" : "(_tmp >> 1)",
                           left ? "1" : std::format("(1ULL << ({} - 1))", _length), gen_mask(_length));
    };

    std::string shift_code;
    if (_has_dir) {
        shift_code = std::format("{0}\n        }} else {{\n{1}", make_shift(false), make_shift(true));
        shift_code = std::format("        if (_dir) {{\n{}\n        }}", shift_code);
    }
    else {
        shift_code = make_shift(true);
    }

    std::string update;
    if (_has_clr) {
        update = std::format(R"(    if (_clr) {{ dcs_memset({0}.stages, 0, {1}); }}
    else if (_rising) {{
{2}
    }})",
                             st, bytes, shift_code);
    }
    else {
        update = std::format(R"(    if (_rising) {{
{1}
    }})",
                             st, shift_code);
    }

    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.stages", st), _length));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1} {2}{3}
    bool _rising = (_clk && !{4}.prev_clk);
{5}
    {4}.prev_clk = _clk;
{6}
}})",
                       funcName_seq(), read_sin, read_clk, read_opt, st, update, write_q);
}


// ============================================================
// clone() 实现 — 用于复合元件展开
// ============================================================

std::unique_ptr<Component> DFlipFlop::clone(const std::string &n) const {
    return std::make_unique<DFlipFlop>(n, _bit_width, _has_en, _has_rst, _has_preset);
}
std::unique_ptr<Component> TFlipFlop::clone(const std::string &n) const {
    return std::make_unique<TFlipFlop>(n, _bit_width);
}
std::unique_ptr<Component> JKFlipFlop::clone(const std::string &n) const {
    return std::make_unique<JKFlipFlop>(n, _bit_width);
}
std::unique_ptr<Component> Register::clone(const std::string &n) const {
    return std::make_unique<Register>(n, _bit_width, _has_en, _has_rst);
}
std::unique_ptr<Component> Latch::clone(const std::string &n) const {
    return std::make_unique<Latch>(n, _bit_width);
}
std::unique_ptr<Component> Counter::clone(const std::string &n) const {
    return std::make_unique<Counter>(n, _bit_width, _has_load, _has_en, _has_updown, _has_clr);
}
std::unique_ptr<Component> ShiftRegister::clone(const std::string &n) const {
    return std::make_unique<ShiftRegister>(n, _length, _has_dir, _has_clr);
}

// ============================================================
// ClockGen — 时钟发生器
// ============================================================

ClockGen::ClockGen(const std::string &name, int high_ticks, int low_ticks) :
    SequentialComponent(name, "clkgen"), _high(high_ticks), _low(low_ticks), _period(high_ticks + low_ticks) {
    if (high_ticks < 1 || low_ticks < 1 || _period < 2)
        throw std::invalid_argument("高低电平 tick 数必须 ≥1");
    setParam("high_ticks", std::to_string(high_ticks));
    setParam("low_ticks", std::to_string(low_ticks));
    addOutput("clk", 1, false, true);
}

std::string ClockGen::genStructDef() const {
    return std::format(R"(typedef struct {{
    int _cnt;
}} {};)",
                       stateTypeName());
}

std::string ClockGen::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string ClockGen::genInitCode() const {
    return std::format("    {}._cnt = 0;", stateVarName());
}

std::string ClockGen::genFuncDef_seq() const {
    int o_nid = outputs()[0]->netId();
    auto st = stateVarName();
    std::string write_o;
    if (o_nid >= 0)
        write_o = std::format("    {}\n", genOutputWrite(0, "_clk", 1));
    else
        write_o = "    // 输出悬空\n";
    return std::format(R"(static void {}() {{
    int _pos = {}._cnt;
    uint8_t _clk = (_pos < {}) ? 1 : 0;
    if (++_pos >= {}) _pos = 0;
    {}._cnt = _pos;
{}
}})",
                       funcName_seq(), st, _high, _period, st, write_o);
}

std::unique_ptr<Component> ClockGen::clone(const std::string &n) const {
    return std::make_unique<ClockGen>(n, _high, _low);
}

} // namespace dsc
