// virtio-blk：块设备。镜像来自宿主文件或电路里的 ROM。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Component.h"
#include "dcs/components/platform/VirtioCommon.h"

namespace dsc {

class VirtioBlk : public SequentialComponent {
public:
    struct State; // 定义在 .cpp

    struct BackendOps {
        void *ctx = nullptr;
        void (*init)(void *ctx) = nullptr;
        void (*deinit)(void *ctx) = nullptr;
        void (*reset)(void *ctx) = nullptr;
        // 按 64 位字节偏移读写镜像，返回 1 成功
        uint32_t (*read)(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) = nullptr;
        uint32_t (*write)(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) = nullptr;
        // 介质字节数，决定设备容量和只读特性位
        uint64_t size = 0;
    };

    explicit VirtioBlk(const std::string &name, const std::string &path = "");
    ~VirtioBlk() override;

    bool prepare(std::string &error) override;
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

    // JIT 回调：state 指向 VirtioBlk::State
    static uint32_t ioAccess(void *state, uint32_t off_lo, uint32_t off_hi, uint8_t *buf, uint32_t len, uint32_t write);

private:
    virtio_gen::DeviceSpec makeSpec() const;

    std::unique_ptr<State> _state;
    std::string _path;
    bool _prepared = false;
};

} // namespace dsc
