// RegBridge — Wishbone 从设备 → 通用寄存器接口主设备
//
// 把一笔 Wishbone 事务转换成元件那套 addr/wdata/we/en → rdata/ready 握手：
//   en 只在事务起始拉一拍，元件在之后任意拍给出 ready，桥接器随即回 ack。
// 全部逻辑在 JIT 里，和元件之间没有函数调用边界。
//
// Wishbone 侧：wb_adr_i/wb_dat_i/wb_we_i/wb_sel_i/wb_stb_i/wb_cyc_i → wb_dat_o/wb_ack_o
// 寄存器侧：addr(8)/wdata(32)/we/en ← rdata(32)/ready
//
// 复位会清掉在途事务（busy/ack 归零）。复位后主设备若仍保持 stb，桥会重新发起
// 一笔事务；主设备已经撤销 stb 的话这笔事务就丢了，所以 circuit_reset 必须和
// Wishbone 主设备的复位同步。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class RegBridge : public SequentialComponent {
public:
    explicit RegBridge(const std::string &name);
    ~RegBridge() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;
};

} // namespace dsc
