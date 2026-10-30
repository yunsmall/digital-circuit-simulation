// WishboneMemory — Wishbone 从设备存储器
//
// 访存逻辑直接生成进 JIT 代码（纯 JIT 模式）：每拍一次的回调调用实测占掉
// 仿真时间的两成以上，内联后省掉调用约定与指针数组间接。
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
};

} // namespace dsc
