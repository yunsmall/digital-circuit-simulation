// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

// 直接继承 Component：要把 Verilator 模型指针放进 genStateDecl，
// 而 CombinationalComponent 把 genStateDecl 标成了 final
class Counter8 : public Component {
public:
    explicit Counter8(const std::string &name);
    ~Counter8() override;

    bool hasCombinational() const override { return false; }
    bool hasSequential() const override { return true; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    struct State;

private:
    std::unique_ptr<State> _state;
};

} // namespace dsc
