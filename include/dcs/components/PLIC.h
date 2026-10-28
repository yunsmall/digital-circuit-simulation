// PLIC — RISC-V 平台级中断控制器（单 context 简化版）
//
// Wishbone 从设备，寄存器布局与标准 PLIC 一致：
//   0x000000 + 4*id   priority[id]   （id >= 1）
//   0x001000 + 4*id   pending[id]    （只读，反映已锁存的源）
//   0x002000 + 4*id   enable[id]
//   0x200000          threshold
//   0x200004          claim/complete
//
// 中断源由 irq_src 输入的每个 bit 提供（bit0 对应源 1）。claim 返回优先级最高的
// 待处理源并把它移入 in_service，complete 写回源号后才允许它再次触发。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class PLIC : public SequentialComponent {
public:
    explicit PLIC(const std::string &name, int num_sources = 8);
    ~PLIC() override;

    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    // 源号 0 保留，pending/enable/in_service 用 uint32_t 的 bit(id) 表示，
    // 因此最多只能表示源 1..31
    static constexpr int MAX_SOURCES = 31;

    struct PlicState {
        uint32_t priority[MAX_SOURCES + 1]; // 下标即源号，0 号未用
        uint32_t enable;                    // 每 bit 对应一个源
        uint32_t threshold;
        uint32_t pending;    // 已到达、等待 claim 的源
        uint32_t in_service; // 已 claim、等待 complete 的源
        uint32_t num_sources;
        uint32_t latched_dout; // 响应数据要保持到事务结束，主设备在 ack 后才采样
        bool prev_stb;
    };
    PlicState _state;
    int _num_sources = 8;
    bool _prepared = false;

    static void plicTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs);
    static void plicReset(void *state_ptr, int num_sources);
};

} // namespace dsc
