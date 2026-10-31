// FloatPrint — 时钟上升沿以人类可读格式打印浮点输入值
#pragma once
#include "dcs/Component.h"

namespace dsc {

class FloatPrint : public SequentialComponent {
public:
    // precision: 32 (float) 或 64 (double)
    FloatPrint(const std::string &name, int precision = 32);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
};

} // namespace dsc
