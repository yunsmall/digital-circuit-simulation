// RegFile — 多读口 / 多写口的寄存器堆
//
// 读是组合的（地址变了数据立刻变），写在时钟上升沿生效。同拍读写同一个地址时
// 读到的是写之前的值，和真实寄存器堆的行为一致。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class RegFile : public Component {
public:
    // bit_width: 数据位宽（1~64）
    // depth: 寄存器个数（2~256）
    // num_read_ports / num_write_ports: 读口（1~4）/ 写口（1~2）数量
    RegFile(const std::string &name, int bit_width, int depth, int num_read_ports, int num_write_ports);

    bool hasCombinational() const override { return true; }
    bool hasSequential() const override { return true; }

    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_comb() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int addrBits() const;
    int _bit_width;
    int _depth;
    int _read_ports;
    int _write_ports;
};

} // namespace dsc
