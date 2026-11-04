// Print — 仿真监控元件，时钟上升沿打印输入值
//
// 默认输出到 stdout；GUI 可以按实例绑定 TextSink 接管这个元件的输出，
// 不影响其它 print 实例。
#pragma once
#include "dcs/Component.h"
#include "dcs/components/io/TextSink.h"

namespace dsc {

class Print : public SequentialComponent {
public:
    // bit_width: 输入位宽（1~64）
    // ascii: true 时按字符输出（串口字节流用），false 时按十六进制打印便于看数值
    // en: 十六进制模式下是电平门控（clk 上升沿且 en 为高才输出）；字符模式下是
    //     "有效字节"选通，按 en 的上升沿输出（UartTerm 的 valid 只高一拍，且不一定
    //     和 clk 对齐）。en 不接时按恒使能处理，字符模式退回 clk 上升沿
    Print(const std::string &name, int bit_width, bool ascii = false);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    // 实例级绑定；必须在开始 tick 之前设置。传空 Sink 即恢复 stdout 输出
    void setSink(const TextSink &sink, void *ctx);
    void clearSink() { setSink({}, nullptr); }

    // JIT 回调入口（对应十六进制模式 / 字符模式）
    void emitValue(const uint8_t *data, int bytes);
    void emitChar(unsigned char c);

private:
    int _bit_width;
    bool _ascii;
    TextSink _sink{};
    void *_sink_ctx = nullptr;
};

} // namespace dsc
