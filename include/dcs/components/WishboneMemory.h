// WishboneMemory — Wishbone 从设备存储器
//
// 访存逻辑全部在 C++ 回调中实现（不受 TCC 优化限制），
// JIT C 代码仅负责引脚 ↔ 回调函数的字节搬运。
//
// 时序: stb 上升沿当拍完成读写，下一拍 ack=1+dat 传播到主设备
//
// Wishbone 从设备引脚:
//   输入: clk(1), wb_adr_i(32), wb_dat_i(32), wb_we_i(1),
//          wb_sel_i(4), wb_stb_i(1), wb_cyc_i(1)
//   输出: wb_dat_o(32), wb_ack_o(1)
//
// IStorage: 外部可直接读写内存（加载程序、检查结果）
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"
#include "dcs/IStorage.h"

namespace dsc {

class WishboneMemory : public SequentialComponent, public IStorage {
public:
    // addr_width: 地址位宽，容量 = 2^addr_width 字节
    WishboneMemory(const std::string &name, int addr_width = 16, int data_width = 32);
    ~WishboneMemory() override;

    // ---- IStorage ----
    const uint8_t *readOnlyMemPtr() const override { return _mem.data(); }
    uint8_t *writableMemPtr() override { return _mem.data(); }
    size_t memSize() const override { return _mem.size(); }

    // ---- Component 接口 ----
    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override { return genInitCode(); }
    std::string genDeinitCode() const override { return ""; }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    int _addr_width;
    int _data_width;
    int _depth;
    std::vector<uint8_t> _mem;
    bool _prepared = false;

    // 访存状态（C 侧 JIT 代码会嵌入指针）
    struct MemState {
        // mem/mem_size/data_bytes 是构造时确定的，复位不能动它们；
        // 运行状态从 rt_offset 开始，复位时整段清零。用偏移量清零而不是回调，
        // 是为了不引入额外的 JIT 符号——实测那会让每个电路多出一倍的编译开销。
        uint8_t *mem;
        size_t mem_size;
        int data_bytes;
        bool prev_stb;       // 上一拍 stb（上升沿检测）
        bool prev_we;        // 上一拍 we（用于检测 AMO 的 we 翻转）
        bool ack_pending;    // 有应答待发送（保持 ack 直到 stb 拉低）
        uint32_t latched_dout;  // 已锁存的读数据（ack 期间保持）
        uint32_t latched_addr;  // 已锁存的地址（用于 AMO 写阶段）
    };
    // 运行状态从这里的偏移开始，复位时整段清零（mem/mem_size/data_bytes 是构造时常量，不能清）
    static constexpr size_t rtOffset() { return offsetof(MemState, prev_stb); }
    static constexpr size_t rtSize() { return sizeof(MemState) - offsetof(MemState, prev_stb); }
    MemState _mstate;

    // ---- C++ 回调（注册为 JIT 符号） ----
    static void memTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs);
};

} // namespace dsc
