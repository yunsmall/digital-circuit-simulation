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
    // IStorage: 统一返回内存指针和大小
    const uint8_t *memPtr() const override { return _mem.data(); }
    size_t memSize() const override { return _mem.size(); }

    // addr_width:  地址位宽（1~12），深度 = 1 << addr_width
    // data_width:  数据位宽（1~64）
    // num_read_ports:  读端口数（1~4）
    // num_write_ports: 写端口数（1~2）
    // read_latency: 读延迟（0~15 tick），0=组合读，N=N拍后数据有效且 rd_valid 置 1
    MultiPortRAM(const std::string &name, int addr_width, int data_width, int num_read_ports, int num_write_ports,
                 int read_latency = 0);

    std::string genStructDef() const override {
        return "";
    }
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    int _addr_width, _data_width, _num_read, _num_write, _read_latency, _rd_stages, _depth;
    std::vector<uint8_t> _mem; // 实际存储（每槽 16 字节对齐）
};

} // namespace dsc
