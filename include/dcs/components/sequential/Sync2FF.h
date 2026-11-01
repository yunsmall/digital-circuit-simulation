// Sync2FF — 跨时钟域两级触发器同步器
//
// 目的时钟域用两个背靠背的触发器采样异步输入，把亚稳态概率压到可接受范围。
// 确定性仿真里没有亚稳态，行为就是两拍延迟，但结构语义要保留，否则跨时钟域
// 设计在仿真里会"看起来能用"、上板出问题。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Sync2FF : public SequentialComponent {
public:
    // bit_width: 同步的数据位宽（1~64）
    Sync2FF(const std::string &name, int bit_width);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

} // namespace dsc
