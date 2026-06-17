// Shifter — 移位器实现
#include "dcs/components/Shifter.h"
#include <bit>
#include <format>

namespace dsc {

static const char *shift_mode_str(ShiftMode m) {
    switch (m) {
        case ShiftMode::LSL:
            return "lsl";
        case ShiftMode::LSR:
            return "lsr";
        case ShiftMode::ASR:
            return "asr";
    }
    return "lsl";
}

Shifter::Shifter(const std::string &name, int bit_width, ShiftMode mode) :
    CombinationalComponent(name, shift_mode_str(mode)), _bit_width(bit_width),
    _mode(mode) {
    setParam("bit_width", std::to_string(bit_width));
    // 移位量位宽 = ceil(log2(bit_width))，最小 1 位
    int shift_bits = std::max(1, std::bit_width((unsigned) bit_width) - 1);
    addInput("in", bit_width);
    addInput("shift", shift_bits);
    addOutput("out", bit_width);
}

std::string Shifter::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int sh_nid = inputs()[1]->netId();
    int sh_nw = sh_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    auto mask = gen_mask(_bit_width);

    std::string shift_op;
    if (_mode == ShiftMode::LSL) {
        shift_op = std::format(R"(
    uint64_t _out = (_in << _sh) & {};)", mask);
    } else if (_mode == ShiftMode::LSR) {
        shift_op = std::format(R"(
    uint64_t _out = _in >> _sh;)");
    } else { // ASR
        // 符号扩展到 int64_t，利用编译器的算术右移
        shift_op = std::format(R"(
    uint64_t _out;
    if (_in & (1ULL << {})) {{
        uint64_t _u = _in | ~{};
        _out = ((uint64_t)((int64_t)_u >> _sh)) & {};
    }} else {{
        _out = _in >> _sh;
    }})", _bit_width - 1, mask, mask);
    }

    return std::format(R"(static void {}() {{
    {}
    {}
    {}
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bit_width, in_nw, "_in"),
                       gen_read_wire(sh_nid, inputs()[1]->bit_width(), sh_nw, "_sh"), shift_op,
                       genOutputWrite(0, "_out", _bit_width));
}

std::unique_ptr<Component> Shifter::clone(const std::string &n) const {
    return std::make_unique<Shifter>(n, _bit_width, _mode);
}

} // namespace dsc
