// SignExt — 符号扩展元件
//
// 将有符号数从 src_width 位扩展到 dst_width 位，
// 保持数值不变：低 src_width 位直接复制，高位全部填充符号位（MSB）。
//
// 引脚: in（src_width 位） → out（dst_width 位）
#pragma once
#include "dcs/Component.h"

namespace dsc {

class SignExt : public CombinationalComponent {
public:
    // src_width: 源位宽（1~63）
    // dst_width: 目标位宽（src_width+1 ~ 64）
    SignExt(const std::string &name, int src_width, int dst_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _src, _dst;
};

} // namespace dsc
