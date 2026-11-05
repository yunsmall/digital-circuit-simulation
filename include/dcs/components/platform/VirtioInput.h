// virtio-input：evdev 事件设备。宿主 pushInputEvent 塞事件，JIT 把它们搬进
// 驱动提交的可写缓冲区。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Component.h"
#include "dcs/components/platform/VirtioCommon.h"

namespace dsc {

class VirtioInput : public SequentialComponent {
public:
    struct State; // 定义在 .cpp

    struct InputEvent {
        uint16_t type;
        uint16_t code;
        int32_t value;
    };

    struct BackendOps {
        void *ctx = nullptr;
        void (*init)(void *ctx) = nullptr;
        void (*deinit)(void *ctx) = nullptr;
        void (*reset)(void *ctx) = nullptr;
    };

    explicit VirtioInput(const std::string &name);
    ~VirtioInput() override;

    void simInit() override;
    void simDeinit() override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    void setBackend(const BackendOps &ops);

    // push 一个 evdev 事件；返回 false 表示环满
    bool pushInputEvent(const InputEvent &ev);

    // 取走一条 guest 发来的状态事件（LED 之类）；没有时返回 false
    bool popStatusEvent(InputEvent &ev);

private:
    virtio_gen::DeviceSpec makeSpec() const;

    std::unique_ptr<State> _state;
    // init 当重置重复调用时后端 init 不能执行第二次
    bool _init_called = false;
};

} // namespace dsc
