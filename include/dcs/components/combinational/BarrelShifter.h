// BarrelShifter — 桶形移位器
//
// 支持左移/右移（逻辑/算术），移位量可变。
//
// 引脚:
//   in    [in]  数据输入 (bit_width)
//   amt   [in]  移位量 (shift_bits = ceil(log2(bit_width)))
//   dir   [in]  方向 (1位, 0=左移 1=右移)
//   arith [in]  算术模式 (1位, 仅右移时有效: 0=逻辑移位 1=算术移位)
//   out   [out] 移位结果 (bit_width)
//
// 左移: out = in << amt（低位补0）
// 右移逻辑: out = in >> amt（高位补0）
// 右移算术: out = in >> amt（高位补符号位）
#pragma once
#include "dcs/Component.h"

namespace dsc {

class BarrelShifter : public CombinationalComponent {
public:
    // bit_width: 数据位宽（2~64）
    BarrelShifter(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bw, _sh_bits;
};

} // namespace dsc
