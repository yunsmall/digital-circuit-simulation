// Parity — 奇偶校验位生成
//
// 输入数据里 1 的个数：even 模式让"{数据, 校验位}"共偶数个 1，odd 模式相反。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Parity : public CombinationalComponent {
public:
    // mode_odd: false = 偶校验，true = 奇校验
    Parity(const std::string &name, int bit_width, bool mode_odd);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _mode_odd;
};

} // namespace dsc
