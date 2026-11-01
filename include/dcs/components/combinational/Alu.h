// ALU — 带操作码的多功能运算单元（74181 那类数据通路核心）
//
// 一个元件覆盖加减/逻辑/移位/比较/乘除，省得在网表里堆一堆单功能运算器。
// 操作码是 4 位输入，高 12 种有定义，其余保留为 0。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Alu : public CombinationalComponent {
public:
    // bit_width: 操作数位宽（1~64）
    Alu(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

} // namespace dsc
