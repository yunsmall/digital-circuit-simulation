// Display — 显示类元件的统一宿主接口
//
// 元件本身不画任何东西：JIT 只在值变化时把新值交给宿主，GUI 通过
// Display::setSink() 注册一次就能收到所有显示件的更新；也可以直接
// 遍历电路里的 Display 元件用 value() 拉取当前值（每帧轮询即可）。
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

    // 进程级唯一；传空 Sink 即注销
    static void setSink(const Sink &sink, void *ctx);

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
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    // JIT 回调入口
    void notifyFromJit(uint64_t v);

private:
    struct State;
    std::unique_ptr<State> _state;
    int _bit_width;
    Kind _kind;
    int _digits;
};

} // namespace dsc
