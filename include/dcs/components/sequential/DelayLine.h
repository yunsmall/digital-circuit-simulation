// DelayLine — N 级延时线，输入延迟 N tick 后出现在输出
#pragma once
#include "dcs/Component.h"

namespace dsc {

class DelayLine : public SequentialComponent {
public:
    // bit_width: 数据位宽（1–64）
    // depth: 延迟深度（1–256），默认 1
    // use_clock: 是否使用时钟触发（默认 true）；false 时每 tick 直接读输入移位
    DelayLine(const std::string &name, int bit_width, int depth = 1, bool use_clock = true);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    int _depth;
    bool _use_clock;
};

} // namespace dsc
