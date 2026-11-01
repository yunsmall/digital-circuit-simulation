// Debounce — 按键/开关去抖
//
// 输入连续 stable_ticks 个时钟沿保持不变才更新输出；中间任何一拍回到原值就清零。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Debounce : public SequentialComponent {
public:
    // bit_width: 输入位宽（1~64）
    // stable_ticks: 需要稳定多少个时钟沿才接受新值（1~65535）
    Debounce(const std::string &name, int bit_width, int stable_ticks);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    int _stable_ticks;
};

} // namespace dsc
