// Shifter — 移位器实现
#include "dcs/ComponentFactory.h"
#include "dcs/components/combinational/Shifter.h"
#include <bit>
#include <cmath>
#include <format>
#include <stdexcept>

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
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    // 移位量要能表示 0..bit_width-1：bit_width(x)-1 等于 floor(log2)，非 2 的幂位宽会少一位
    int shift_bits = (int) std::ceil(std::log2((double) bit_width));
    if (shift_bits < 1) shift_bits = 1;
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

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"lsl",
                        {{"bit_width", "8"}},
                        {"in", "shift"},
                        {"out"},
                        dsc::ComponentCategory::DataPath, "逻辑左移，out=in<<shift，低位补0"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Shifter>(n, dsc::paramInt(p, "bit_width", 8), dsc::ShiftMode::LSL);
                       });

DCS_REGISTER_COMPONENT(
    {"lsr",
                        {{"bit_width", "8"}},
                        {"in", "shift"},
                        {"out"},
                        dsc::ComponentCategory::DataPath, "逻辑右移，out=in>>shift，高位补0"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Shifter>(n, dsc::paramInt(p, "bit_width", 8), dsc::ShiftMode::LSR);
                       });

DCS_REGISTER_COMPONENT(
    {"asr",
                        {{"bit_width", "8"}},
                        {"in", "shift"},
                        {"out"},
                        dsc::ComponentCategory::DataPath, "算术右移，out=in>>shift，高位补符号位"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Shifter>(n, dsc::paramInt(p, "bit_width", 8), dsc::ShiftMode::ASR);
                       });
} // namespace
