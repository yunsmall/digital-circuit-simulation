// RingCounter — 环形 / 约翰逊计数器
//
// 环形：反馈最高位到最低位，one-hot 循环；约翰逊：反馈最高位的反，产生 2N 个状态。
// 复位后环形回到最低位为 1，约翰逊回到全 0。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class RingCounter : public SequentialComponent {
public:
    // length: 位数（2~64）
    // johnson: true 时是约翰逊计数器（反馈取反）
    RingCounter(const std::string &name, int length, bool johnson);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _length;
    bool _johnson;
};

} // namespace dsc
