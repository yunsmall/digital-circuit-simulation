// DelayLine — N 级延时线实现
#include "dcs/components/DelayLine.h"
#include <format>
#include <stdexcept>

namespace dsc {

DelayLine::DelayLine(const std::string &name, int bit_width, int depth, bool use_clock) :
    SequentialComponent(name, "delay"), _bit_width(bit_width), _depth(depth), _use_clock(use_clock) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    if (depth < 1 || depth > 256)
        throw std::invalid_argument("延时深度必须在1-256之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("depth", std::to_string(depth));
    setParam("use_clock", use_clock ? "1" : "0");
    addInput("in", bit_width);
    if (use_clock)
        addInput("clk", 1);
    addOutput("out", bit_width, false, true);
}

std::string DelayLine::genStructDef() const {
    return std::format(R"(typedef struct {{
    uint8_t _val[{}][16];
    int _pos;
    uint8_t prev_clk;
}} {};)",
                       _depth, stateTypeName());
}

std::string DelayLine::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string DelayLine::genInitCode() const {
    return std::format("    dcs_memset(&{}, 0, sizeof({}));", stateVarName(), stateTypeName());
}

std::string DelayLine::genFuncDef_seq() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int out_nid = outputs()[0]->netId();
    auto st = stateVarName();
    int bytes = byte_count(_bit_width);

    std::string read_in = gen_read_wire(in_nid, _bit_width, in_nw, "_in_val");
    std::string write_out = genOutputWrite(0, std::format("{0}._val[{0}._pos]", st), _bit_width);

    if (_use_clock) {
        int clk_nid = inputs()[1]->netId();
        std::string clk_read = clk_nid >= 0
                                       ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                       : "    bool _clk = false;";

        return std::format(R"(static void {0}() {{
    {1}
    {2}
    bool _rising = (_clk && !{3}.prev_clk);
    if (_rising) {{
        // 先输出当前槽的旧值（即 depth 个 tick 前的输入），再写入新值
        {6}
        dcs_memcpy({3}._val[{3}._pos], &_in_val, {4});
        if (++{3}._pos >= {5}) {3}._pos = 0;
    }}
    {3}.prev_clk = _clk;
}})",
                           funcName_seq(), // {0}
                           read_in, // {1}
                           clk_read, // {2}
                           st, // {3}
                           bytes, // {4}
                           _depth, // {5}
                           write_out // {6}
        );
    }
    else {
        // 无时钟模式：每 tick 直接读输入，移位后输出
        return std::format(R"(static void {0}() {{
    {1}
    // 先输出旧值，再写入新值移位
    {2}
    dcs_memcpy({3}._val[{3}._pos], &_in_val, {4});
    if (++{3}._pos >= {5}) {3}._pos = 0;
}})",
                           funcName_seq(), // {0}
                           read_in, // {1}
                           write_out, // {2}
                           st, // {3}
                           bytes, // {4}
                           _depth // {5}
        );
    }
}

std::unique_ptr<Component> DelayLine::clone(const std::string &n) const {
    return std::make_unique<DelayLine>(n, _bit_width, _depth, _use_clock);
}

} // namespace dsc
