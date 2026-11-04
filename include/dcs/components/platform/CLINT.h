// CLINT — RISC-V 核心局部中断器 (Core Local Interruptor)
//
// Wishbone 从设备，提供:
//   - mtime (64-bit 单调计数器, 只读, 偏移 0xBFF8)
//   - mtimecmp (64-bit 定时器比较值, 读写, 偏移 0x4000)
//   - msip (32-bit 软件中断, 读写, 偏移 0x0000)
//
// 当 mtime >= mtimecmp 时，mti 输出为 1
// 当 msip != 0 时，msi 输出为 1
//
// 所有寄存器按 32-bit 小端访问（RV32 标准）
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class CLINT : public SequentialComponent {
public:
    CLINT(const std::string &name);

    // ---- Component 接口 ----
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
};

} // namespace dsc
