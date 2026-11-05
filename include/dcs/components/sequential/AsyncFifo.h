// AsyncFifo — 双时钟域异步 FIFO（格雷码指针 + 两级同步器）
//
// 写域只看 wr_clk，读域只看 rd_clk，两边的满/空标志都基于对方同步过来的
// 格雷码指针。指针每次只跳一位，跨域采样不会出现多位同时变化。
// 仿真里没有亚稳态，但同步器结构保留：好处是能如实反映"跨域看到指针要晚两拍"，
// 上层电路不会依赖一个现实中不存在的零延迟。
//
// rd_data 只在 !rd_empty 时有效，空时保持最后一次读出的内容（不清 0），
// 和同步 FIFO 的 dout 一致，下游必须用空标志门控
#pragma once
#include "dcs/Component.h"

namespace dsc {

class AsyncFifo : public SequentialComponent {
public:
    // bit_width: 数据位宽（1~64）；depth: 深度，必须是 2 的幂（2~256）
    AsyncFifo(const std::string &name, int bit_width, int depth);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    int _depth;
};

} // namespace dsc
