// Reduction — 归约门（Verilog &a / |a / ^a）
//
// N 位输入归约为 1 位输出：
//   AND:  全 1 输出 1
//   OR:   有 1 输出 1
//   XOR:  奇数个 1 输出 1（奇偶校验）
#pragma once
#include "dcs/Component.h"

namespace dsc {

enum class ReductionOp { AND, OR, XOR };

class Reduction : public CombinationalComponent {
public:
    Reduction(const std::string &name, int bit_width, ReductionOp op);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    ReductionOp _op;
};

} // namespace dsc
