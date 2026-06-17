// FIFO — 同步先入先出队列
//
// 与 Memory 不同，FIFO 保持写入顺序，读时返回最早写入的数据。
// 使用循环缓冲区实现，读写指针自动管理。
//
// 引脚:
//   din    [in]  写入数据 (data_width)
//   wr_en  [in]  写使能 (1位)，clk 上升沿且 wr_en=1 且 !full 时写入
//   rd_en  [in]  读使能 (1位)，clk 上升沿且 rd_en=1 且 !empty 时读出
//   clk    [in]  时钟 (1位)，上升沿触发
//   rst    [in]  异步复位 (1位)，可选，高有效
//   dout   [out] 读出数据 (data_width)
//   full   [out] 满标志 (1位)
//   empty  [out] 空标志 (1位)
//
// 参数:
//   data_width: 数据位宽（1~64）
//   depth:      FIFO 深度（2~65536，必须为 2 的幂）
#pragma once
#include <cstdint>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class FIFO : public SequentialComponent {
public:
    FIFO(const std::string &name, int data_width, int depth, bool has_rst = false);

    int dataWidth() const {
        return _data_width;
    }
    int fifoDepth() const {
        return _depth;
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
    int _data_width, _depth;
    bool _has_rst;
    std::vector<uint8_t> _mem;
};

} // namespace dsc
