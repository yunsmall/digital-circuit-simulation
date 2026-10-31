// virtio-net：两个队列（0=收包，1=发包）。包在宿主与设备之间走 State 里的环，
// JIT 直接读写，热路径没有回调。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Component.h"
#include "dcs/components/platform/VirtioCommon.h"

namespace dsc {

class VirtioNet : public SequentialComponent {
public:
    struct State; // 定义在 .cpp

    struct BackendOps {
        void *ctx = nullptr;
        void (*init)(void *ctx) = nullptr;
        void (*deinit)(void *ctx) = nullptr;
        void (*reset)(void *ctx) = nullptr;
        // 取一个待接收的帧（返回长度，0 表示没有）/ 发出一个帧
        uint32_t (*recv_pkt)(void *ctx, uint8_t *buf, uint32_t max_len) = nullptr;
        void (*send_pkt)(void *ctx, const uint8_t *buf, uint32_t len) = nullptr;
    };

    explicit VirtioNet(const std::string &name);
    ~VirtioNet() override;

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

    // 往 RX 队列塞一个以太网帧（不含 virtio_net_hdr）；返回 false 表示环满
    bool pushRxPacket(const uint8_t *data, uint32_t len);
    // 取走设备发出的一个帧（不含 virtio_net_hdr）；返回长度，0 表示没有
    uint32_t popTxPacket(uint8_t *buf, uint32_t max_len);
    // 和 BackendOps 的 recv_pkt/send_pkt 对接一次；宿主在两次 tick 之间调用
    void pollBackend();

private:
    virtio_gen::DeviceSpec makeSpec() const;

    std::unique_ptr<State> _state;
};

} // namespace dsc
