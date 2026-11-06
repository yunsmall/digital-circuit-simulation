// SPI — 8 位主机，模式 0（空闲低、上升沿采样）
//
// 只实现模式 0（CPOL=0/CPHA=0）：模式 1/2/3 没有控制位也没有实现，接从机时
// 必须按模式 0 接。
//
// Wishbone 从设备，寄存器：
//   0x00 TXDATA  待发送字节
//   0x04 RXDATA  收到的字节
//   0x08 STATUS  bit0=忙, bit1=一次传输完成
//   0x0C CONTROL bit0=写 1 启动一次传输
//   0x10 CLKDIV  每半个位周期的 tick 数
//
// cs 低有效，sclk 由 clkdiv 分频产生；发送在 sclk 下降沿移出，接收在上升沿采样。
// clk 输入引脚保留未用：sclk 按 tick 分频，状态机不检测 clk 边沿
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class SPI : public SequentialComponent {
public:
    explicit SPI(const std::string &name, int ns_per_period = 20, int sclk_period_ns = 80);
    ~SPI() override;

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
    int _sclk_period_ns = 80;
    int _clkdiv = 4;
};

} // namespace dsc
