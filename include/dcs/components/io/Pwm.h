// Pwm — 单通道脉宽调制输出（总线无关的寄存器接口）
//
// 输入引脚：clk, addr(8), wdata(32), we, en
// 输出引脚：rdata(32), ready, pwm(1)
//
// 寄存器（8 位地址）：
//   0x00 PERIOD  一个周期的 tick 数
//   0x04 DUTY    每周期内高电平的 tick 数（>= PERIOD 时恒高）
//   0x08 CTRL    bit0=使能；关闭时输出恒低
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class Pwm : public SequentialComponent {
public:
    explicit Pwm(const std::string &name);
    ~Pwm() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;
};

} // namespace dsc
