// VirtioMmio — virtio-mmio（modern v2）设备
//
// 结构对齐真实 SoC：
//   * 从端口（wb_*）：CPU 读写 MMIO 寄存器；
//   * 主端口（m_*）：设备发起 Wishbone 事务，访问 guest 内存里的
//     descriptor/avail/used 环和数据缓冲。
//
// 寄存器、队列状态机和总线事务全部生成进 JIT（和 DMA/PLIC 同一套做法）：
// 每拍要么推进一步队列状态机，要么推进一步主端口事务，没有跨模块回调。
// 唯一的 C++ 回调是镜像读写——那必须碰宿主文件/ROM 数据。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class VirtioMmio : public SequentialComponent {
public:
    enum class Kind { Blk, Console, Rng, Input, Net, NineP };

    struct State; // 仅保存镜像后端，定义在 .cpp

    explicit VirtioMmio(const std::string &name, Kind kind = Kind::Blk, const std::string &path = "");
    ~VirtioMmio() override;

    bool prepare(std::string &error) override;
    void simInit() override;
    void simDeinit() override;
    // path 为空时把电路里的 ROM 当只读镜像：ROM 是设备自己的固件存储，
    // 不是 guest 内存，直接读它的数据不算绕过总线
    void bindCircuit(Circuit *circuit) override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override;
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 镜像读写：off 是 64 位字节偏移，write=1 时把 buf 写进镜像。
    // 返回 1 成功，0 失败（越界/只读/文件错误）
    static uint32_t ioAccess(void *state, uint32_t off_lo, uint32_t off_hi, uint8_t *buf, uint32_t len, uint32_t write);

    // 后端函数指针表：第三方实现自己的后端只要填这张表，不需要继承任何类。
    // 调用发生在设备事务边界（每次 I/O / 每个数据包），不在每拍热路径上
    struct BackendOps {
        void *ctx = nullptr;
        // 生命周期：仿真开始 / 仿真结束 / guest 把 status 写 0（设备复位）
        void (*init)(void *ctx) = nullptr;
        void (*deinit)(void *ctx) = nullptr;
        void (*reset)(void *ctx) = nullptr;
        // blk：按字节偏移读写整个镜像，返回 1 成功
        uint32_t (*read)(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) = nullptr;
        uint32_t (*write)(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) = nullptr;
        // net：取一个待接收的包（返回长度，0 表示没有）/ 发出一个包
        uint32_t (*recv_pkt)(void *ctx, uint8_t *buf, uint32_t max_len) = nullptr;
        void (*send_pkt)(void *ctx, const uint8_t *buf, uint32_t len) = nullptr;
        // 9p：一个请求进、一个响应出，返回响应长度
        uint32_t (*transact)(void *ctx, const uint8_t *req, uint32_t req_len,
                             uint8_t *resp, uint32_t resp_max) = nullptr;
    };
    // 覆盖默认后端（文件/ROM 镜像）。必须在 compile() 之前调用
    void setBackend(const BackendOps &ops);

    // ---- 宿主侧接口：外部世界往设备里送数据 ----
    // 事件/数据包在没有 tick 时才 push，仿真单线程，JIT 直接读 State 里的队列
    struct InputEvent {
        uint16_t type;
        uint16_t code;
        int32_t value;
    };
    // push 一个 evdev 事件；返回 false 表示队列满
    bool pushInputEvent(const InputEvent &ev);

private:

private:
    // 各设备的队列状态机（每个设备一个实现文件，共享寄存器/主端口/队列骨架）
    std::string genBlkDone() const;
    std::string genBlkStart() const;
    std::string genRngDone() const;
    std::string genRngStart() const;

    std::unique_ptr<State> _state;
    Kind _kind = Kind::Blk;
    std::string _path;
    bool _prepared = false;
};

} // namespace dsc
