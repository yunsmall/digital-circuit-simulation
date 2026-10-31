// virtio-rng：熵源设备。随机数在 JIT 里用 xorshift64 生成。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Component.h"
#include "dcs/components/platform/VirtioCommon.h"

namespace dsc {

class VirtioRng : public SequentialComponent {
public:
    struct State; // 定义在 .cpp

    struct BackendOps {
        void *ctx = nullptr;
        void (*init)(void *ctx) = nullptr;
        void (*deinit)(void *ctx) = nullptr;
        void (*reset)(void *ctx) = nullptr;
    };

    explicit VirtioRng(const std::string &name);
    ~VirtioRng() override;

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

private:
    virtio_gen::DeviceSpec makeSpec() const;

    std::unique_ptr<State> _state;
};

} // namespace dsc
