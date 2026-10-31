// PriorityEncoder — 优先编码器
//
// 多个输入中找出最高优先级（索引最大）的活跃位。
// 与普通 Encoder 不同：多输入同时为 1 时输出最高优先级索引，行为定义明确。
//
// 引脚:
//   in0..in_{N-1} [in]  各 1 位，共 num_inputs 个输入
//   out            [out] log2(num_inputs) 位，最高活跃位索引
//   valid          [out] 1 位，是否有任何输入为 1
#pragma once
#include "dcs/Component.h"

namespace dsc {

class PriorityEncoder : public CombinationalComponent {
public:
    // num_inputs: 输入数量（2~64），输出位宽 = ceil(log2(num_inputs))
    PriorityEncoder(const std::string &name, int num_inputs);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _n, _out_bits;
};

} // namespace dsc
