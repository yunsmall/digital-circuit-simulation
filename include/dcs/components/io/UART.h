// UART — 16550A 兼容的 8N1 串口（stride 4，对应设备树 reg-shift=2、reg-io-width=4）
//
// Wishbone 从设备，寄存器：
//   0x00 RBR(读)/THR(写)/DLL   LCR.DLAB=1 时访问 DLL
//   0x04 IER/DLM               LCR.DLAB=1 时访问 DLM
//   0x08 IIR(读)/FCR(写)
//   0x0C LCR
//   0x10 MCR
//   0x14 LSR                   bit0=DR（收到数据） bit5=THRE bit6=TEMT
//   0x18 MSR
//   0x1C SCR
//
// tx/rx 是串行线，按 baud_div 个 tick 一位输出/采样，与真实 UART 的时序关系一致，
// 只是这里的"时间单位"是仿真 tick。DLL/DLM 只保存不影响分频（分频由构造参数固定）。
// clk 输入引脚保留未用：收发状态机按 tick 推进，不检测 clk 边沿；接不接都不影响行为
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class UART : public SequentialComponent {
public:
    // ns_per_period: 该元件 clk 引脚一个时钟周期多少 ns
    // baud: 波特率（bit/s），内部换算成每比特时钟周期数
    explicit UART(const std::string &name, int ns_per_period = 20, std::uint64_t baud = 115200);
    ~UART() override;

    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    int _ns_per_period = 20;
    std::uint64_t _baud = 115200;
    int _cycles_per_bit = 2;
};

} // namespace dsc
