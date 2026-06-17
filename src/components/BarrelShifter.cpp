// BarrelShifter — 桶形移位器实现
#include "dcs/components/BarrelShifter.h"
#include <cmath>
#include <format>
#include <stdexcept>

namespace dsc {

BarrelShifter::BarrelShifter(const std::string &name, int bit_width) :
    CombinationalComponent(name, "barrel"), _bw(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    if (bit_width < 2 || bit_width > 64)
        throw std::invalid_argument("位宽必须在2-64之间");
    // 移位量位宽: ceil(log2(bit_width))
    _sh_bits = (int) std::ceil(std::log2(bit_width));
    if (_sh_bits < 1)
        _sh_bits = 1;

    addInput("in", bit_width);
    addInput("amt", _sh_bits);
    addInput("dir", 1); // 0=左移 1=右移
    addInput("arith", 1); // 0=逻辑 1=算术（仅右移有效）
    addOutput("out", bit_width);
}

std::string BarrelShifter::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int amt_nid = inputs()[1]->netId();
    int dir_nid = inputs()[2]->netId();
    int arith_nid = inputs()[3]->netId();
    int o_nid = outputs()[0]->netId();

    std::string read_dir = dir_nid >= 0 ? std::format("    bool _dir = false; dcs_memcpy(&_dir, _w[{}], 1);", dir_nid)
                                        : "    bool _dir = false;"; // 默认左移
    std::string read_arith =
            arith_nid >= 0 ? std::format("    bool _arith = false; dcs_memcpy(&_arith, _w[{}], 1);", arith_nid)
                           : "    bool _arith = false;";

    std::string write;
    if (o_nid >= 0)
        write = std::format("    {}\n    _c_oe_{}_0 = true;\n", genOutputWrite(0, "_out", _bw), _id);
    else
        write = "    // 输出悬空\n";

    return std::format(R"(static void {}() {{
    {}
    {} _amt = 0; dcs_memcpy(&_amt, _w[{}], {});
    _amt &= {};
    {}
    {}
    {} _out = 0;
    if (_dir) {{
        // 右移
        if (_arith && (_in >> {})) {{
            // 算术右移: 高位补符号位
            _out = (_in >> _amt) | (({} << ({} - _amt)) & {});
        }} else {{
            _out = _in >> _amt;
        }}
    }} else {{
        // 左移
        _out = (_in << _amt) & {};
    }}
{}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bw, in_nw, "_in"), c_int_type(_sh_bits),
                       amt_nid >= 0 ? amt_nid : 0, byte_count(_sh_bits), gen_mask(_sh_bits), read_dir, read_arith,
                       c_int_type(_bw),
                       _bw - 1, // 检查 MSB（符号位）
                       gen_mask(_bw), // 全1掩码
                       _bw, // 补位数
                       gen_mask(_bw), // 结果掩码
                       gen_mask(_bw), // 左移结果掩码
                       write);
}

std::unique_ptr<Component> BarrelShifter::clone(const std::string &n) const {
    return std::make_unique<BarrelShifter>(n, _bw);
}

} // namespace dsc
