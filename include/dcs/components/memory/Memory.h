// Memory — 带读写延迟的可寻址 RAM 元件
//
// 内存数据分配在 C++ 侧，地址直接写入生成的 C 代码。
// 外界通过 data() 直接读写。
//
// 输入: addr, data_in, we(1bit), clk(1bit)
// 输出: data_out, busy(1bit)
//
// 读延迟: 地址改变后，data_out 经过 read_latency 个周期才有效
// 写延迟: we=1 的上升沿后，经过 write_latency 个周期才写入
// busy=1 表示有写操作在进行中
#pragma once
#include <cstdint>
#include <vector>
#include "dcs/Component.h"
#include "dcs/IStorage.h"

namespace dsc {

class Memory : public SequentialComponent, public IStorage {
public:
    // IStorage: 字节寻址，始终可写
    const uint8_t *readOnlyMemPtr() const override { return _mem.data(); }
    uint8_t *writableMemPtr() override { return _mem.data(); }
    size_t memSize() const override { return (size_t)_depth; }
    // addr_width: 地址总线位宽，容量 = 2^addr_width 字节
    // data_width: 数据位宽（1~64）
    // read_latency: 读延迟（0=组合逻辑直出）
    // write_latency: 写延迟（0=上升沿直写）
    Memory(const std::string &name, int addr_width, int data_width, int read_latency = 0, int write_latency = 0);

    int depth() const {
        return _depth; // 容量（字节数）
    }
    int addrWidth() const {
        return _addr_width;
    }
    int dataWidth() const {
        return _data_width;
    }
    const uint8_t *data() const {
        return _mem.data();
    }
    uint8_t *data() {
        return _mem.data();
    }

    std::string genStructDef() const override {
        return "";
    }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _addr_width, _data_width, _depth;
    int _read_latency, _write_latency;
    int _rd_stages, _wr_stages; // 流水线级数（= latency + 1）
    std::vector<uint8_t> _mem;
};

// ============================================================
} // namespace dsc
