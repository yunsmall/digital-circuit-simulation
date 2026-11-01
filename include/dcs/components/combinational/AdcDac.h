// ADC / DAC — 模拟量与数字量之间的线性转换
//
// 都用 32 位 float 表示模拟电压。DAC 把数字量按满量程映射到 [0, vref]；
// ADC 反过来做量化，越界输入饱和到端点。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class Dac : public CombinationalComponent {
public:
    // bit_width: 数字输入位宽（1~32）
    // vref: 满量程电压，数字最大值对应 vref
    Dac(const std::string &name, int bit_width, double vref);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    double _vref;
};

class Adc : public CombinationalComponent {
public:
    // bit_width: 数字输出位宽（1~32）
    // vref: 满量程电压
    Adc(const std::string &name, int bit_width, double vref);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    double _vref;
};

} // namespace dsc
