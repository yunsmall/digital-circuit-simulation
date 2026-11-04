// FloatPrint — 时钟上升沿以人类可读格式打印浮点输入值
//
// 默认输出到 stdout；GUI 可以按实例绑定 TextSink 接管这个元件的输出，
// 不影响其它 fprint 实例。
#pragma once
#include "dcs/Component.h"
#include "dcs/components/io/TextSink.h"

namespace dsc {

class FloatPrint : public SequentialComponent {
public:
    // precision: 32 (float) 或 64 (double)
    FloatPrint(const std::string &name, int precision = 32);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    // 实例级绑定；必须在开始 tick 之前设置。传空 Sink 即恢复 stdout 输出
    void setSink(const TextSink &sink, void *ctx);
    void clearSink() { setSink({}, nullptr); }

    // JIT 回调入口
    void emitValue(const uint8_t *data);

private:
    int _precision;
    TextSink _sink{};
    void *_sink_ctx = nullptr;
};

} // namespace dsc
