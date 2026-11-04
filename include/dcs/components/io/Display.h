// Display — 显示类元件，每个实例单独绑定宿主回调
//
// 元件本身不画任何东西：JIT 只在值变化时把新值交给宿主。绑定是实例级的，
// 所以同一个进程里可以有多个电路/窗口，各自挂自己的渲染器，互不覆盖；
// 也可以不绑定，直接遍历电路里的 Display 元件用 value() 拉取当前值。
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "dcs/Component.h"

namespace dsc {

class Display : public SequentialComponent {
public:
    enum class Kind { Led, SevenSeg, Text };

    struct Sink {
        // owner 是元件本身，GUI 可以据此拿到 name()/kind()；在仿真线程里调用
        void (*on_change)(void *ctx, const Display *owner, uint64_t value) = nullptr;
    };

    // 实例级绑定：只影响这个元件，不改变其它 Display 的行为。
    // 在仿真线程里调用回调，所以必须在开始 tick 之前设置；传空 Sink 即解绑。
    // 回调里不要把 owner 存出去：元件被 removeComponent 后指针就悬空了
    void setSink(const Sink &sink, void *ctx);
    void clearSink() { setSink({}, nullptr); }

    // bit_width: 输入位宽（1~64）
    // digits: 数码管位数，仅 SevenSeg 有意义，1~8
    Display(const std::string &name, int bit_width, Kind kind, int digits);
    ~Display() override;

    uint64_t value() const;
    Kind kind() const { return _kind; }
    int digits() const { return _digits; }

    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    // JIT 回调入口
    void notifyFromJit(uint64_t v);
    // 复位时把宿主侧缓存清零，保证 reset() 之后 value() 不会读到旧值
    void resetHostValue();

private:
    struct State;
    std::unique_ptr<State> _state;
    int _bit_width;
    Kind _kind;
    int _digits;
    Sink _sink{};
    void *_sink_ctx = nullptr;
};

} // namespace dsc
