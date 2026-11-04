// MultiPortRAM — 参数化多端口 RAM（同步写，读可选流水线延迟）
//
// 端口配置：num_read_ports 个读端口 + num_write_ports 个写端口
// 每 tick 先输出读数据（旧值），再在时钟上升沿执行写入（新值下 tick 可见）
// 读流水线自动推进，输出 rd_valid 指示当前数据是否有效
// 用途：寄存器堆（2读1写）、冯诺依曼统一内存（2读1写或更多）
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "dcs/Component.h"
#include "dcs/IStorage.h"

namespace dsc {

class MultiPortRAM : public SequentialComponent, public IStorage {
public:
    // 以下裸指针仅供仿真框架，元件禁止调用（见 IStorage.h）。未 prepare 时返回 nullptr
    const uint8_t *readOnlyMemPtr() const override { return isPrepared() ? _mem.data() : nullptr; }
    uint8_t *writableMemPtr() override { return isPrepared() ? _mem.data() : nullptr; }
    size_t memSize() const override { return (size_t)_depth; }

    // addr_width:  地址总线位宽，容量 = 2^addr_width 字节
    // data_width:  数据位宽（1~64）
    // num_read_ports:  读端口数（1~4）
    // num_write_ports: 写端口数（1~2）
    // read_latency: 读延迟 tick 数，0=组合读
    MultiPortRAM(const std::string &name, int addr_width, int data_width, int num_read_ports, int num_write_ports,
                 int read_latency = 0);

    // 存储分配放在 prepare：构造只做参数校验和建引脚
    bool prepare(Context &ctx, std::string &error) override;

    std::string genStructDef() const override {
        return "";
    }
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    // 只在 prepare() 里调用
    void allocateMem();
    int _addr_width, _data_width, _num_read, _num_write, _read_latency, _rd_stages, _depth;
    std::vector<uint8_t> _mem; // 字节寻址存储（_depth 字节）
};

} // namespace dsc
