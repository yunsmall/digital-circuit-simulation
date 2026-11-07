// Axi4MasterAdapter — 总线中立的存储器事务接口 → AXI4 主设备
//
// 与 wb_master_adapter 对应：CPU 只产生 mem_* 事务（单次读写），本元件把它翻译
// 成 AXI4 的单拍事务（AxLEN=0、AxSIZE=4、BURST=INCR），处理未对齐地址的字节
// 通道搬移，收到 B/R 响应后回 mem_ack。
//
// AXI 的输入输出之间不允许组合路径（A3.1.1），因此应答比 Wishbone 直连多一拍：
// 所有握手都在 ACLK 上升沿推进，mem_ack 也是寄存器输出。
#pragma once
#include <string>

#include "dcs/Component.h"

namespace dsc {

class Axi4MasterAdapter : public SequentialComponent {
public:
    explicit Axi4MasterAdapter(const std::string &name, int id_width = 4);
    ~Axi4MasterAdapter() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    int _id_width;
};

} // namespace dsc
