// Watchdog — 看门狗定时器（总线无关的寄存器接口）
//
// 输入引脚：clk, addr(8), wdata(32), we, en
// 输出引脚：rdata(32), ready, irq(1)
//
// 寄存器（8 位地址）：
//   0x00 CTRL     bit0=使能
//   0x04 TIMEOUT  超时 tick 数
//   0x08 FEED     写任意值喂狗（计数清零、中断撤下）
//   0x0C STATUS   只读：bit0=已超时
//
// irq 超时后保持高，直到喂狗或关闭。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class Watchdog : public SequentialComponent {
public:
    explicit Watchdog(const std::string &name);
    ~Watchdog() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;
};

} // namespace dsc
