// I2C — 主机，完成一次"起始 + 地址 + 数据 + 停止"的写事务
//
// Wishbone 从设备，寄存器：
//   0x00 TXDATA   要发送的数据字节
//   0x04 RXDATA   最近一次读到的字节（当前实现只做写事务，保留给读事务扩展）
//   0x08 STATUS   bit0=忙, bit1=事务完成, bit2=从机未应答(NACK)
//   0x0C CONTROL  bit0=写 1 发起一次写事务
//   0x10 ADDR     7 位从机地址（bit7..1）
//   0x14 CLKDIV   每半个 SCL 周期的 tick 数
//
// sda_out/sda_oe 组成开漏输出：oe=0 时释放总线（外部上拉为高），sda_in 读回线电平。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class I2C : public SequentialComponent {
public:
    explicit I2C(const std::string &name, int clkdiv = 8);
    ~I2C() override;

    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    int _clkdiv = 8;
};

} // namespace dsc
