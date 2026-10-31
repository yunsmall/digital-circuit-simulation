// Shifter — 移位器（逻辑/算术）
//
// 逻辑左移 LSL: out = in << shift
// 逻辑右移 LSR: out = in >> shift（高位补 0）
// 算术右移 ASR: out = in >>> shift（高位补符号位）
#pragma once
#include "dcs/Component.h"

namespace dsc {

enum class ShiftMode { LSL, LSR, ASR };

class Shifter : public CombinationalComponent {
public:
    // bit_width: 数据位宽（1~64）
    // mode: 移位模式
    Shifter(const std::string &name, int bit_width, ShiftMode mode);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    ShiftMode _mode;
};

} // namespace dsc
