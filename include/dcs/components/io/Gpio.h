// Gpio — 32 位通用输入输出口（总线无关的寄存器接口）
//
// 输入引脚：clk, addr(8), wdata(32), we, en, gpio_in(32)
// 输出引脚：rdata(32), ready, gpio_out(32)
//
// 寄存器（8 位地址）：
//   0x00 DIR   bit=1 该引脚为输出
//   0x04 OUT   输出电平
//   0x08 IN    输入电平（只读）
//
// 外部驱动/读取直接用 gpio_in / gpio_out 两根线网。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class Gpio : public SequentialComponent {
public:
    explicit Gpio(const std::string &name);
    ~Gpio() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;
};

} // namespace dsc
