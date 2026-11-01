// BCD/十六进制 → 七段码译码器（7447/7448 那类）
//
// 输出位定义：bit0..bit6 = a..g，bit7 = 小数点。
// 共阳数码管要低电平点亮，把 active_low 置 1，段码整体取反。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Bcd2Seg : public CombinationalComponent {
public:
    // hex_mode: 0 时只译 0~9，A~F 显示为全灭；1 时按十六进制译出 A~F
    // active_low: 1 时输出低有效（共阳数码管）
    Bcd2Seg(const std::string &name, bool hex_mode = false, bool active_low = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    bool _hex_mode;
    bool _active_low;
};

} // namespace dsc
