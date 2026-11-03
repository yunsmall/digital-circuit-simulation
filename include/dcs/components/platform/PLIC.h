// PLIC — RISC-V 平台级中断控制器（两个 context：0=M 态、1=S 态）
//
// Wishbone 从设备，寄存器布局与标准 PLIC 一致：
//   0x000000 + 4*id   priority[id]   （id >= 1，3 位，0 表示永不中断）
//   0x001000 + 4*word pending[word]  （只读，32 位位图，位 N%32 表示源 N）
//   0x002000 + 0x80*ctx + 4*word  enable[ctx][word]（32 位位图）
//   0x200000 + 0x1000*ctx         threshold[ctx]
//   0x200004 + 0x1000*ctx         claim/complete[ctx]
//
// 中断源由 irq_src 输入的每个 bit 提供（bit0 对应源 1）。claim 返回该 context
// 已使能且待处理中优先级最高的源并把它移入 in_service（claim 不看 threshold），
// complete 写回源号后才允许它再次触发。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class PLIC : public SequentialComponent {
public:
    explicit PLIC(const std::string &name, int num_sources = 8);
    ~PLIC() override;

    bool prepare(Context &ctx, std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    // 因此最多只能表示源 1..31
    static constexpr int MAX_SOURCES = 31;

    int _num_sources = 8;
    bool _prepared = false;
};

} // namespace dsc
