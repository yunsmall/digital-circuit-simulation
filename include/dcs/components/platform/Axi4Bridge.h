// Axi4Bridge — AXI4 从设备 → Wishbone 主设备，支持 burst
//
// AxiLiteBridge 只覆盖单拍寄存器访问；本元件实现完整 AXI4 事务：INCR/WRAP/FIXED
// 三种 burst、AxLEN/AxSIZE、窄传输与未对齐首拍、WSTRB、AWID/ARID 原样回送
// （BID/RID）、WLAST/RLAST。每个 beat 翻译成一次 Wishbone 单次传输，事务之间
// 串行执行（read data reordering depth = 1，符合 A5.2.1 对从设备的最低要求）。
//
// 时序按 A3.1.1：AXI 接口的输入与输出之间不允许组合路径，所有握手状态都在
// ACLK 上升沿推进，输出保持整个周期。因此比 Wishbone 直连多一拍，但协议正确。
//
// 复位：项目惯例 rst 高有效；复位时撤销 BVALID/RVALID 并丢弃在途事务。
// 错误响应：本桥不产生 SLVERR/DECERR。地址译码失败由互连的默认从设备回答；
// Wishbone 从设备不回 ack 时事务会一直等待（和 CPU 主设备行为一致），接线错误
// 由互连看门狗诊断。
#pragma once
#include <string>

#include "dcs/Component.h"

namespace dsc {

class Axi4Bridge : public SequentialComponent {
public:
    Axi4Bridge(const std::string &name, int data_width = 32, int addr_width = 32, int id_width = 4);
    ~Axi4Bridge() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    std::string genAdvanceCode() const;

    int _data_width;
    int _addr_width;
    int _id_width;
};

} // namespace dsc
