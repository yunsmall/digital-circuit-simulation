// virtio-net：两个队列（0=收包，1=发包）。包在宿主与设备之间走 State 里的环，
// JIT 直接读写，热路径没有回调。
#pragma once
#include <cstdint>
#include <map>
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
        // 申请后端上下文。可以在这里起后台线程和宿主交互，
        // 仿真开始（Circuit::init → simInit）时调用
        void *(*create)(void *cfg) = nullptr;
        // 释放上下文，必须停掉 create 里起的线程；仿真销毁时调用
        void (*destroy)(void *ctx) = nullptr;
        // 数据面：取一帧给 guest / 送一帧给外部
        uint32_t (*recv_pkt)(void *ctx, uint8_t *buf, uint32_t max_len) = nullptr;
        void (*send_pkt)(void *ctx, const uint8_t *buf, uint32_t len) = nullptr;
        // guest 把 status 写 0
        void (*reset)(void *ctx) = nullptr;
        // JIT 往 TX 环推入一个包后调用，唤醒阻塞中的后端线程。
        // 每个包一次，不是每拍
        void (*kick)(void *ctx) = nullptr;
    };

    // 后端注册表：第三方在自己的 .cpp 里用 DCS_REGISTER_VIRTIO_NET_BACKEND 自注册，
    // 元件的 backend 参数只存名字，JSON 往返后能自动恢复
    static void registerBackend(const std::string &name, const BackendOps &ops);

    explicit VirtioNet(const std::string &name, const std::string &backend = "none",
                       std::map<std::string, std::string> backend_opts = {});
    ~VirtioNet() override;

    bool prepare(std::string &error) override;
    void simInit() override;
    void simDeinit() override;

    // 后端名字随元件参数一起序列化，宿主加载 JSON 后照名字接上实现
    const std::string &backendName() const { return _backend; }

    // 后端私有选项：JSON 里写成嵌套对象（"backend_opts": {"hostfwd": "..."}），
    // 加载时拍平成 backend_opts.<key> 参数键再由元件按前缀摘出来。具体 key 的含义
    // 由后端自己定义（slirp 认 hostfwd，tap 认网卡名……），元件不解释，
    // 也不会因为后端新增选项而改动。约定见 dcs/net/BackendOpts.h
    const std::map<std::string, std::string> &backendOpts() const { return _backend_opts; }

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
    std::string _backend;
    std::map<std::string, std::string> _backend_opts;
    BackendOps _ops;

    // JIT 侧入口：从 State 里取后端上下文转发 kick
    static void kickThunk(void *state);
};

// 后端自注册：放在后端实现文件的全局作用域。
// 名字是字符串字面量，所以变量名用 __COUNTER__ 生成，不能拿 name 直接拼
#define DCS_VNET_CAT2(a, b) a##b
#define DCS_VNET_CAT(a, b) DCS_VNET_CAT2(a, b)
#define DCS_REGISTER_VIRTIO_NET_BACKEND(name, ops_expr)                                              \
    namespace {                                                                                      \
    [[maybe_unused]] const bool DCS_VNET_CAT(_dcs_vnet_backend_, __COUNTER__) =                      \
            (::dsc::VirtioNet::registerBackend(name, ops_expr), true);                               \
    }

} // namespace dsc
