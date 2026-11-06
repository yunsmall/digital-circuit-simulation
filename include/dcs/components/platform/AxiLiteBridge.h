// AxiLiteBridge — AXI4-Lite 从设备 → Wishbone 主设备
//
// 让 AXI4-Lite 主设备（如 Zynq 的 PS 端口、各类 AXI IP）访问本项目的 Wishbone 总线。
// 写通道 AW/W 各自独立握手，收齐地址和数据后发一次 Wishbone 写，B 通道回响应；
// 读通道 AR 收到即发一次 Wishbone 读，拿到数据后在 R 通道返回。
// 同一时刻只处理一笔事务（无 outstanding），足够应付寄存器访问。
//
// 时序：状态机只在 clk 上升沿推进，握手输出保持整个时钟周期，边沿触发的
// AXI 主设备才能在上升沿采到 READY/BVALID/RVALID。
//
// 复位：genResetCode 等同 genInitCode——复位会撤销在途事务（BVALID/RVALID 拉低、
// 已收到的 AW/AR 丢弃）。AXI 要求主从一起复位，circuit_reset 必须和外部 AXI 主
// 设备的复位同时发生；单独复位桥会让等响应的一侧永远等下去。
//
// 错误响应：桥不产生 SLVERR/DECERR。Wishbone 从设备不回 ack 时事务一直等（和 CPU
// 主设备遇到悬空从设备的行为一致），由互连的卡死看门狗诊断，接线错误应在设计期
// 由 wishbone_ic 的告警发现。地址对齐由 AXI 主设备保证（AXI4-Lite 没有 AxSIZE，
// 桥按 WSTRB 直接映射到 Wishbone SEL），非对齐窄传输会得到未定义结果。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class AxiLiteBridge : public SequentialComponent {
public:
    explicit AxiLiteBridge(const std::string &name);
    ~AxiLiteBridge() override;

    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

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
