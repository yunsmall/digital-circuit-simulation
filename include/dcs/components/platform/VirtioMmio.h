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
    enum class Kind { Blk, Console };

    struct State; // 仅保存镜像后端，定义在 .cpp

    explicit VirtioMmio(const std::string &name, Kind kind = Kind::Blk, const std::string &path = "");
    ~VirtioMmio() override;

    bool prepare(std::string &error) override;
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

private:
    std::unique_ptr<State> _state;
    Kind _kind = Kind::Blk;
    std::string _path;
    bool _prepared = false;
};

} // namespace dsc
