// Print — 仿真监控元件，时钟上升沿打印输入值到 stdout
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Print : public SequentialComponent {
public:
    // bit_width: 输入位宽（1~64）
    Print(const std::string &name, int bit_width);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

} // namespace dsc
