// DMA — 存储器到存储器的块搬运控制器
//
// 两个独立的 Wishbone 接口：
//   cfg_* : 从设备侧，CPU 用它配置源地址、目的地址、长度并启动
//   m_*   : 主设备侧，搬运时由自己发起读/写事务，接在 Interconnect 的主端口上
//
// 寄存器：
//   0x00 SRC     源地址
//   0x04 DST     目的地址
//   0x08 LEN     字节数（按字搬运，必须是 4 的倍数）
//   0x0C CTRL    写 1 启动
//   0x10 STATUS  bit0=忙, bit1=完成
//
// 搬运按字进行：读一个字再写一个字，源或目的地址递增 4。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class DMA : public SequentialComponent {
public:
    explicit DMA(const std::string &name);
    ~DMA() override;

    bool prepare(Context &ctx, std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    bool _prepared = false;
};

} // namespace dsc
