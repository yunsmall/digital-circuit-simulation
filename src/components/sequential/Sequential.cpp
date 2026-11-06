#include "dcs/ComponentFactory.h"
#include "dcs/components/sequential/Sequential.h"
#include <format>
#include <stdexcept>

namespace dsc {

namespace {
// 最高字节里有效位的掩码：位宽是 8 的倍数时整字节都有效。
// 状态数组是 uint8_t，直接 &= gen_mask(bit_width) 会先提升再截回 8 位，
// 超出位宽的高位（如 12 位时的 0xF000）根本清不掉
unsigned tailByteMask(int bit_width) {
    const int tail = bit_width % 8;
    return tail == 0 ? 0xFFu : (1u << tail) - 1u;
}
} // namespace

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
    addInput("clk", 1, PinEdge::Rising);
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
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    int en_nid = _has_en ? inputs()[2]->netId() : -1;
    int rst_nid = _has_rst ? inputs()[_has_en ? 3 : 2]->netId() : -1;
    // 引脚顺序是 d(0), clk(1), [en], [rst], [pre]，所以 pre 前面固定有两个引脚
    int pre_nid = _has_preset ? inputs()[2 + _has_en + _has_rst]->netId() : -1;

    std::string read_d = gen_read_wire(d_nid, _bit_width, d_nw, "_dval");

    std::string read_opt;
    if (_has_en && en_nid >= 0)
        read_opt += std::format("\n    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid);
    else if (_has_en)
        read_opt += "\n    bool _en = true;";
    else
        read_opt += "\n    bool _en = true;";
    // 引脚存在但悬空时也要声明变量：update 分支会引用它，否则生成的 C 代码编译失败
    if (_has_rst)
        read_opt += rst_nid >= 0 ? std::format("\n    bool _rst = false; dcs_memcpy(&_rst, _w[{}], 1);", rst_nid)
                              : std::string("\n    bool _rst = false;");
    if (_has_preset)
        read_opt += pre_nid >= 0 ? std::format("\n    bool _pre = false; dcs_memcpy(&_pre, _w[{}], 1);", pre_nid)
                              : std::string("\n    bool _pre = false;");

    std::string edge = std::format("bool _rising = {};", genGetWireEdge(1, PinEdge::Rising));

    // 预置位整字节写 0xFF 后，最后一字节要按有效位掩掉高位（位宽是 8 的倍数
    // 时整字节有效，不用掩）
    const std::string preset_mask =
            _bit_width % 8 == 0 ? std::string()
                                : std::format("        {}.q[{} - 1] &= 0x{:X}u;\n", st, bytes, tailByteMask(_bit_width));

    std::string update;
    if (_has_rst && _has_preset) {
        update = std::format(R"(    if (_rst) {{
        dcs_memset({0}.q, 0, {1});
    }} else if (_pre) {{
        dcs_memset({0}.q, 0xFF, {1});
{2}    }} else if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes, preset_mask);
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
{2}    }} else if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes, preset_mask);
    }
    else {
        update = std::format(R"(    if (_rising && _en) {{
        dcs_memcpy({0}.q, &_dval, {1});
    }})",
                             st, bytes);
    }

    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空";

    return std::format(R"(static void {0}(void) {{
    {1}{2}
    {3}
    {4}
    {5}
}})",
                       funcName_seq(), read_d, read_opt, edge, update, write_q);
}

// ============================================================
// T 触发器
// ============================================================
TFlipFlop::TFlipFlop(const std::string &name, int bit_width) : SequentialComponent(name, "tff"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("t", 1);
    addInput("clk", 1, PinEdge::Rising);
    addOutput("q", bit_width, false, true);
}
std::string TFlipFlop::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t q[16]; }} {};)", stateTypeName());
}
std::string TFlipFlop::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string TFlipFlop::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string TFlipFlop::genFuncDef_seq() const {
    int t_nid = inputs()[0]->netId();
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    std::string read_t =
            t_nid >= 0 ? std::format("bool _t = false; dcs_memcpy(&_t, _w[{}], 1);", t_nid) : "bool _t = false;";
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1}
    bool _rising = {2};
    if (_rising && _t) {{
        for (int _i = 0; _i < {3}; _i++) {4}.q[_i] = ~{4}.q[_i];
        {4}.q[{3} - 1] &= 0x{5:X}u;
    }}
{6}
}})",
                       funcName_seq(), read_t, genGetWireEdge(1, PinEdge::Rising), bytes, st,
                       tailByteMask(_bit_width), write_q);
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
    addInput("clk", 1, PinEdge::Rising);
    addOutput("q", bit_width, false, true);
}
std::string JKFlipFlop::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t q[16]; }} {};)", stateTypeName());
}
std::string JKFlipFlop::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string JKFlipFlop::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string JKFlipFlop::genFuncDef_seq() const {
    int j_nid = inputs()[0]->netId(), k_nid = inputs()[1]->netId();
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    std::string read_j =
            j_nid >= 0 ? std::format("bool _j = false; dcs_memcpy(&_j, _w[{}], 1);", j_nid) : "bool _j = false;";
    std::string read_k =
            k_nid >= 0 ? std::format("bool _k = false; dcs_memcpy(&_k, _w[{}], 1);", k_nid) : "bool _k = false;";
    std::string write_q;
    if (q_nid >= 0)
        write_q = std::format("    {}\n", genOutputWrite(0, std::format("{}.q", st), _bit_width));
    else
        write_q = "    // 输出悬空\n";
    return std::format(R"(static void {0}(void) {{
    {1} {2}
    bool _rising = {3};
    if (_rising) {{
        if (_j && _k) {{
            for (int _i = 0; _i < {5}; _i++) {4}.q[_i] = ~{4}.q[_i];
            {4}.q[{5} - 1] &= 0x{6:X}u;
        }} else if (_j) {{ dcs_memset({4}.q, 0xFF, {5}); {4}.q[{5} - 1] &= 0x{6:X}u;
        }} else if (_k) {{ dcs_memset({4}.q, 0, {5}); }}
    }}
{7}
}})",
                       funcName_seq(), read_j, read_k, genGetWireEdge(2, PinEdge::Rising), st, bytes,
                       tailByteMask(_bit_width), write_q);
}

// ============================================================
// 寄存器
// ============================================================
Register::Register(const std::string &name, int bit_width, bool has_en, bool has_rst) :
    SequentialComponent(name, "register"), _bit_width(bit_width), _has_en(has_en), _has_rst(has_rst) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("has_en", _has_en ? "1" : "0");
    setParam("has_rst", _has_rst ? "1" : "0");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("d", bit_width);
    addInput("clk", 1, PinEdge::Rising);
    if (_has_en)
        addInput("en", 1);
    if (_has_rst)
        addInput("rst", 1);
    addOutput("q", bit_width, false, true);
}
std::string Register::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t data[16]; }} {};)", stateTypeName());
}
std::string Register::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string Register::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string Register::genFuncDef_seq() const {
    int d_nid = inputs()[0]->netId(), q_nid = outputs()[0]->netId();
    int d_nw = d_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    int en_nid = _has_en ? inputs()[2]->netId() : -1;
    int rst_nid = _has_rst ? inputs()[_has_en ? 3 : 2]->netId() : -1;

    std::string read_d = gen_read_wire(d_nid, _bit_width, d_nw, "_dval");
    std::string read_opt;
    if (_has_en && en_nid >= 0)
        read_opt += std::format("\n    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid);
    else if (_has_en)
        read_opt += "\n    bool _en = true;";
    else
        read_opt += "\n    bool _en = true;";
    if (_has_rst)
        read_opt += rst_nid >= 0 ? std::format("\n    bool _rst = false; dcs_memcpy(&_rst, _w[{}], 1);", rst_nid)
                              : std::string("\n    bool _rst = false;");

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
    {1}{2}
    bool _rising = {3};
{4}
{5}
}})",
                       funcName_seq(), read_d, read_opt, genGetWireEdge(1, PinEdge::Rising), update, write_q);
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
    SequentialComponent(name, "counter"), _bit_width(bit_width), _has_load(has_load), _has_en(has_en),
    _has_updown(has_updown), _has_clr(has_clr) {
    setParam("bit_width", std::to_string(bit_width));
    setParam("has_load", _has_load ? "1" : "0");
    setParam("has_en", _has_en ? "1" : "0");
    setParam("has_updown", _has_updown ? "1" : "0");
    setParam("has_clr", _has_clr ? "1" : "0");
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    addInput("clk", 1, PinEdge::Rising);
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
    return std::format(R"(typedef struct {{ uint8_t count[16]; }} {};)", stateTypeName());
}
std::string Counter::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string Counter::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string Counter::genFuncDef_seq() const {
    int q_nid = outputs()[0]->netId();
    int bytes = byte_count(_bit_width);
    auto st = stateVarName();
    int pi = 1;
    int load_nid = _has_load ? inputs()[pi++]->netId() : -1;
    int din_nid = _has_load ? inputs()[pi++]->netId() : -1;
    int en_nid = _has_en ? inputs()[pi++]->netId() : -1;
    int updown_nid = _has_updown ? inputs()[pi++]->netId() : -1;
    int clr_nid = _has_clr ? inputs()[pi++]->netId() : -1;

    std::string read_opt, load_decl;
    if (_has_load) {
        load_decl = load_nid >= 0 ? std::format("bool _load = false; dcs_memcpy(&_load, _w[{}], 1);", load_nid)
                                  : "bool _load = false;";
        int din_nw = din_nid >= 0 ? inputs()[2]->net()->bit_width() : 0;
        read_opt += std::format("\n    {}\n    {}", load_decl, gen_read_wire(din_nid, _bit_width, din_nw, "_din"));
        load_decl = "";
    }
    // 没有 load 引脚时不要声明 _load：update 分支里根本不会读它，只会产生未使用变量警告

    if (_has_en && en_nid >= 0)
        read_opt += std::format("\n    bool _en = false; dcs_memcpy(&_en, _w[{}], 1);", en_nid);
    else if (_has_en)
        read_opt += "\n    bool _en = true;";
    else
        read_opt += "\n    bool _en = true;";
    if (_has_updown)
        read_opt += updown_nid >= 0 ? std::format("\n    bool _updown = false; dcs_memcpy(&_updown, _w[{}], 1);", updown_nid)
                                   : std::string("\n    bool _updown = false;");
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
        {1}.count[{0} - 1] &= 0x{2:X}u;)",
                              bytes, st, tailByteMask(_bit_width));
    }
    else {
        inc_dec = std::format(R"(        uint16_t _carry = 1;
        for (int _i = 0; _i < {0}; _i++) {{ uint16_t _tmp = {1}.count[_i] + _carry; {1}.count[_i] = (uint8_t)_tmp; _carry = (_tmp >> 8) & 1; }}
        {1}.count[{0} - 1] &= 0x{2:X}u;)",
                              bytes, st, tailByteMask(_bit_width));
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
    {1}
    bool _rising = {2};
    {3}
{4}
{5}
}})",
                       funcName_seq(), read_opt, genGetWireEdge(0, PinEdge::Rising), load_decl, update, write_q);
}

// ============================================================
// 移位寄存器
// ============================================================
ShiftRegister::ShiftRegister(const std::string &name, int length, bool has_dir, bool has_clr) :
    SequentialComponent(name, "shift"), _length(length), _has_dir(has_dir), _has_clr(has_clr) {
    setParam("length", std::to_string(_length));
    setParam("has_dir", _has_dir ? "1" : "0");
    setParam("has_clr", _has_clr ? "1" : "0");
    if (length < 1 || length > 64)
        throw std::invalid_argument("移位寄存器长度必须在1-64之间");
    addInput("sin", 1);
    addInput("clk", 1, PinEdge::Rising);
    if (_has_dir)
        addInput("dir", 1);
    if (_has_clr)
        addInput("clr", 1);
    addOutput("q", length, false, true);
}
std::string ShiftRegister::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t stages[16]; }} {};)", stateTypeName());
}
std::string ShiftRegister::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string ShiftRegister::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}
std::string ShiftRegister::genFuncDef_seq() const {
    int sin_nid = inputs()[0]->netId(), q_nid = outputs()[0]->netId();
    int bytes = byte_count(_length);
    auto st = stateVarName();
    int dir_nid = _has_dir ? inputs()[2]->netId() : -1;
    int clr_nid = _has_clr ? inputs()[_has_dir ? 3 : 2]->netId() : -1;
    int utype = _length == 64 ? 64 : (_length <= 8 ? 8 : _length <= 16 ? 16 : _length <= 32 ? 32 : 64);

    std::string read_sin = sin_nid >= 0 ? std::format("bool _sin = false; dcs_memcpy(&_sin, _w[{}], 1);", sin_nid)
                                        : "bool _sin = false;";
    std::string read_opt;
    if (_has_dir)
        read_opt += dir_nid >= 0 ? std::format("\n    bool _dir = false; dcs_memcpy(&_dir, _w[{}], 1);", dir_nid)
                                : std::string("\n    bool _dir = false;");
    if (_has_clr && clr_nid >= 0)
        read_opt += std::format("\n    bool _clr = false; dcs_memcpy(&_clr, _w[{}], 1);", clr_nid);
    else if (_has_clr)
        read_opt += "\n    bool _clr = false;";

    auto make_shift = [&](bool left) -> std::string {
        // _tmp 必须先清零：memcpy 只覆盖 ceil(长度/8) 字节，位宽不是 8 的倍数时
        // 高位是未初始化的，右移会把它们挪进有效位（读未初始化值本身也是 UB）
        return std::format(R"(        uint{0}_t _tmp = 0;
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
    {1}
    bool _rising = {2};{3}
{4}
{5}
}})",
                       funcName_seq(), read_sin, genGetWireEdge(1, PinEdge::Rising), read_opt, update, write_q);
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

ClockGen::ClockGen(const std::string &name, int high_ticks, int low_ticks, PinEdge tick_edge) :
    SequentialComponent(name, "clkgen"), _high(high_ticks), _low(low_ticks), _period(high_ticks + low_ticks),
    _tick_edge(tick_edge) {
    if (high_ticks < 1 || low_ticks < 1 || _period < 2)
        throw std::invalid_argument("高低电平 tick 数必须 ≥1");
    if (tick_edge != PinEdge::None && tick_edge != PinEdge::Rising && tick_edge != PinEdge::Falling)
        throw std::invalid_argument("clkgen 的 tick_edge 只能是 none/rising/falling");
    setParam("high_ticks", std::to_string(high_ticks));
    setParam("low_ticks", std::to_string(low_ticks));
    setParam("tick_edge", tick_edge == PinEdge::Rising ? "rising" : tick_edge == PinEdge::Falling ? "falling" : "none");
    Pin *out = addOutput("clk", 1, false, true);
    out->setDriveEdge(tick_edge);
}

std::string ClockGen::genStructDef() const {
    // 1/1 就是每拍翻转的 50% 时钟，是最常见的接法：状态只要一位，
    // 不需要 period/位置这些信息
    const char *field = (_high == 1 && _low == 1) ? "uint8_t _clk;" : "int _cnt;";
    return std::format(R"(typedef struct {{
    {}
}} {};)",
                       field, stateTypeName());
}

std::string ClockGen::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string ClockGen::genInitCode() const {
    if (_high == 1 && _low == 1)
        return std::format("    {}._clk = 0;", stateVarName());
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
    // 伪时钟线没有电平语义，固定输出 0；接到它的元件只用 genGetWireEdge
    if (_tick_edge != PinEdge::None)
        return std::format(R"(static void {}() {{
    uint8_t _clk = 0;
{}
}})",
                           funcName_seq(), write_o);
    // 周期 2：初值 0，每拍异或 1，第一拍就是高电平，和计数器的相位一致。
    // 热路径上省掉 ++/比较/回绕分支
    if (_high == 1 && _low == 1)
        return std::format(R"(static void {}() {{
    uint8_t _clk = ({}._clk ^= 1);
{}
}})",
                           funcName_seq(), st, write_o);
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
    return std::make_unique<ClockGen>(n, _high, _low, _tick_edge);
}

// ============================================================
// EdgeDetect
// ============================================================
EdgeDetect::EdgeDetect(const std::string &name, EdgeType edge) :
    SequentialComponent(name, edge == EdgeType::Rising ? "edgedet_rise" : edge == EdgeType::Falling ? "edgedet_fall"
                                                                                                   : "edgedet_both"),
    _edge(edge) {
    addInput("clk", 1, PinEdge::Rising);
    addInput("in", 1);
    addOutput("out", 1, false, true);
}

std::string EdgeDetect::genStructDef() const {
    return std::format(R"(typedef struct {{
    uint8_t prev_in;
    uint8_t out_val;
}} {};)", stateTypeName());
}
std::string EdgeDetect::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string EdgeDetect::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}

std::string EdgeDetect::genFuncDef_seq() const {
    int in_nid = inputs()[1]->netId();
    auto st = stateVarName();

    std::string read_in = in_nid >= 0
                              ? std::format("uint8_t _in = 0; dcs_memcpy(&_in, _w[{}], 1);", in_nid)
                              : "uint8_t _in = 0;";

    std::string cond;
    if (_edge == EdgeType::Rising)
        cond = std::format("(_in && !{}.prev_in)", st);
    else if (_edge == EdgeType::Falling)
        cond = std::format("(!_in && {}.prev_in)", st);
    else
        cond = std::format("(_in != {}.prev_in)", st);

    return std::format(R"(static void {}() {{
    {}
    bool _rising = {};
    if (_rising) {{
        {}.out_val = {} ? 1 : 0;
        {}.prev_in = _in;
    }}
    {}
}})",
                       funcName_seq(), read_in, genGetWireEdge(0, PinEdge::Rising), st, cond, st,
                       genOutputWrite(0, std::format("{}.out_val", st), 1));
}

std::unique_ptr<Component> EdgeDetect::clone(const std::string &n) const {
    return std::make_unique<EdgeDetect>(n, _edge);
}

// ============================================================
// ClockDivider
// ============================================================
ClockDivider::ClockDivider(const std::string &name, int divisor) :
    SequentialComponent(name, "clkdiv"), _divisor(divisor) {
    setParam("divisor", std::to_string(divisor));
    if (divisor < 2)
        throw std::invalid_argument("分频比必须 >= 2");
    // 不声明 PinEdge：分频器也是时钟源，在边沿检测之前执行，自己按电平检测
    // 输入 clk 的上升沿（和 CPU 同一种方式）
    addInput("clk", 1);
    addOutput("out", 1, false, true);
}

std::string ClockDivider::genStructDef() const {
    return std::format(R"(typedef struct {{
    uint8_t prev_clk;
    uint64_t counter;
}} {};)", stateTypeName());
}
std::string ClockDivider::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}
std::string ClockDivider::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}

std::string ClockDivider::genFuncDef_seq() const {
    auto st = stateVarName();
    int d2 = _divisor / 2;
    int clk_nid = inputs()[0]->netId();

    return std::format(R"(static void {}() {{
    {}
    bool _rising = (_clk && !{}.prev_clk);
    {}.prev_clk = _clk;
    if (_rising)
        {}.counter = ({}.counter + 1) % {};
    uint8_t _out = ({}.counter < {}) ? 1 : 0;
    {}
}})",
                       funcName_seq(), gen_read_wire(clk_nid, 1, 1, "_clk"), st, st, st, st, _divisor, st, d2,
                       genOutputWrite(0, "_out", 1));
}

std::unique_ptr<Component> ClockDivider::clone(const std::string &n) const {
    return std::make_unique<ClockDivider>(n, _divisor);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"dff",
     {{"bit_width", "8"}, {"has_en", "0"}, {"has_rst", "0"}, {"has_preset", "0"}},
     {"d", "clk"},
     {"q"},
     dsc::ComponentCategory::Sequential,
     "D触发器，clk上升沿采样d输出到q"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::DFlipFlop>(n, dsc::paramInt(p, "bit_width", 8),
                                                                   dsc::paramInt(p, "has_en", 0) != 0,
                                                                   dsc::paramInt(p, "has_rst", 0) != 0,
                                                                   dsc::paramInt(p, "has_preset", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"tff", {{"bit_width", "8"}}, {"t", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "T触发器，clk上升沿且t=1时翻转"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::TFlipFlop>(n, dsc::paramInt(p, "bit_width", 8));
                       });

DCS_REGISTER_COMPONENT(
    {"jkff", {{"bit_width", "8"}}, {"j", "k", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "JK触发器，j/k控制置位/复位，j=k=1时翻转"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::JKFlipFlop>(n, dsc::paramInt(p, "bit_width", 8));
                       });

DCS_REGISTER_COMPONENT(
    {"register", {{"bit_width", "8"}, {"has_en", "0"}, {"has_rst", "0"}}, {"d", "clk"}, {"q"},
     dsc::ComponentCategory::Sequential, "寄存器，clk上升沿采样d"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Register>(n, dsc::paramInt(p, "bit_width", 8),
                                                                  dsc::paramInt(p, "has_en", 0) != 0,
                                                                  dsc::paramInt(p, "has_rst", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"latch", {{"bit_width", "8"}}, {"d", "en"}, {"q"}, dsc::ComponentCategory::Sequential, "锁存器，en=1时透明，en=0时锁存"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Latch>(n, dsc::paramInt(p, "bit_width", 8));
                       });

DCS_REGISTER_COMPONENT(
    {"counter",
                        {{"bit_width", "8"}, {"has_load", "0"}, {"has_en", "0"}, {"has_updown", "0"}, {"has_clr", "0"}},
                        {"clk"},
                        {"q"},
                        dsc::ComponentCategory::Sequential, "计数器，支持加载/使能/升降方向/清零"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Counter>(n, dsc::paramInt(p, "bit_width", 8), dsc::paramInt(p, "has_load", 0) != 0,
                                                                 dsc::paramInt(p, "has_en", 0) != 0, dsc::paramInt(p, "has_updown", 0) != 0,
                                                                 dsc::paramInt(p, "has_clr", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"shift", {{"length", "8"}, {"has_dir", "0"}, {"has_clr", "0"}}, {"sin", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "移位寄存器，支持串入/方向切换/清零"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::ShiftRegister>(
                                   n, dsc::paramInt(p, "length", 8), dsc::paramInt(p, "has_dir", 0) != 0, dsc::paramInt(p, "has_clr", 0) != 0);
                       });

DCS_REGISTER_COMPONENT(
    {"clkgen", {{"high_ticks", "1"}, {"low_ticks", "1"}, {"tick_edge", "none", {{"none"}, {"rising"}, {"falling"}}}}, {}, {"clk"}, dsc::ComponentCategory::Sequential, "时钟发生器，可设高/低电平持续tick数；tick_edge 非 none 时输出改为每 tick 都是该沿的伪时钟线（无电平语义）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           const std::string e = dsc::paramStr(p, "tick_edge", "none");
                           const dsc::PinEdge pe = e == "rising" ? dsc::PinEdge::Rising
                                                   : e == "falling" ? dsc::PinEdge::Falling
                                                                    : dsc::PinEdge::None;
                           return std::make_unique<dsc::ClockGen>(n, dsc::paramInt(p, "high_ticks", 1), dsc::paramInt(p, "low_ticks", 1), pe);
                       });

DCS_REGISTER_COMPONENT(
    {"edgedet_rise", {{}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential, "上升沿检测，in上升沿时out=1一个周期"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::EdgeDetect>(n, dsc::EdgeType::Rising);
                       });

DCS_REGISTER_COMPONENT(
    {"edgedet_fall", {{}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential, "下降沿检测，in下降沿时out=1一个周期"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::EdgeDetect>(n, dsc::EdgeType::Falling);
                       });

DCS_REGISTER_COMPONENT(
    {"edgedet_both", {{}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential, "双边沿检测，in任意边沿时out=1一个周期"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::EdgeDetect>(n, dsc::EdgeType::Both);
                       });

DCS_REGISTER_COMPONENT(
    {"clkdiv", {{"divisor", "2"}}, {"clk"}, {"out"}, dsc::ComponentCategory::Sequential, "时钟分频器，每N个clk周期翻转一次"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::ClockDivider>(n, dsc::paramInt(p, "divisor", 2));
                       });
} // namespace
