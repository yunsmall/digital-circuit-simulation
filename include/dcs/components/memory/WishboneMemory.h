// WishboneMemory — Wishbone 从设备存储器
//
// 访存逻辑直接生成进 JIT 代码（纯 JIT 模式）：每拍一次的回调调用实测占掉
// 仿真时间的两成以上，内联后省掉调用约定与指针数组间接。
//
// 时序: stb 上升沿当拍完成读写，下一拍 ack=1+dat 传播到主设备
//
// ---- 与 Wishbone B4 的对应关系（classic 模式）----
//   * 最小信号集 ACK_O/CLK_I/CYC_I/STB_I/RST_I 齐全（B4 p.35）；
//   * ACK_O 由 CYC_I & STB_I 产生，随 STB_I 撤销而撤销（RULE 3.35 / RULE 3.50）；
//   * ADR 只到字边界，字节由 SEL 选（RULE 3.100，Figure 3-19）；
//   * 主设备在事务完成前保持 STB/CYC，收到 ACK 才撤销。
// 未实现的信号在规范里是可选或属于其它模式：
//   * ERR_O / RTY_O —— PERMISSION 3.20 / 3.25，标记为 MAY；
//   * STALL_I —— 3.1.3.2 pipelined 模式，classic 不需要；
//   * CTI/BTE burst —— Chapter 4 Registered Feedback 扩展。
//
// 硬件数据引脚与内部暂存都是 32 位，data_width 只支持 8/16/32。
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

    // ---- IStorage：以下裸指针仅供仿真框架，元件禁止调用（见 IStorage.h）。未 prepare 时返回 nullptr ----
    const uint8_t *readOnlyMemPtr() const override { return isPrepared() ? _mem.data() : nullptr; }
    uint8_t *writableMemPtr() override { return isPrepared() ? _mem.data() : nullptr; }
    size_t memSize() const override { return (size_t) _depth; }

    // ---- Component 接口 ----
    bool prepare(Context &ctx, std::string &error) override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genResetCode() const override { return genInitCode(); }
    std::string genDeinitCode() const override { return ""; }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    // 只在 prepare() 里调用
    void allocateMem();
    int _addr_width;
    int _data_width;
    int _depth;
    std::vector<uint8_t> _mem;
};

} // namespace dsc
