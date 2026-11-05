// RTC — goldfish 风格的实时时钟（compatible = "google,goldfish-rtc"）
//
// 总线无关的寄存器接口：addr/wdata/we/en → rdata/ready。接 Wishbone/AXI-Lite
// 时由对应适配器元件转换，元件本身不假设任何总线协议。
//
// 寄存器（8 位地址）：
//   0x00 TIME_LOW / 0x04 TIME_HIGH   当前时间，单位纳秒（Unix epoch）
//   0x08 ALARM_LOW / 0x0C ALARM_HIGH 闹钟时间
//   0x10 IRQ_ENABLED                 写 1 使能闹钟中断
//   0x14 CLEAR_ALARM                 写任意值关闭闹钟并清中断
//
// 时间按每个 tick 递增 ns_per_tick 模拟（默认 100ns）。全部逻辑在 JIT 里。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class RTC : public SequentialComponent {
public:
    explicit RTC(const std::string &name, uint32_t ns_per_tick = 100);
    ~RTC() override;

    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 外部 API：基准时间（Unix 纳秒）。必须在 compile 之前设置，值会被编进 JIT 代码；
    // 写入 setParam，exportJson/fromJson 往返不会丢时间基准
    void setBaseNs(uint64_t ns);
    uint64_t baseNs() const { return _base_ns; }
    uint32_t nsPerTick() const { return _ns_per_tick; }

private:
    uint64_t _base_ns = 0;
    uint32_t _ns_per_tick = 100;
};

} // namespace dsc
