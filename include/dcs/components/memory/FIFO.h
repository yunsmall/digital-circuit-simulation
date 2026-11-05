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
//
// dout 是 FWFT（首字直通）：任何时刻都等于 rd_ptr 指向槽位的内容，
// 所以 empty=1 时它是缓冲区里的残留数据、语义无效，不会清 0，下游必须用 empty
// 门控。这和 Memory/ROM 越界读返回 0 的约定不同，换用存储器时要注意
#pragma once
#include <cstdint>
#include <vector>
#include "dcs/Component.h"
#include "dcs/IStorage.h"

namespace dsc {

class FIFO : public SequentialComponent, public IStorage {
public:
    // 以下裸指针仅供仿真框架，元件禁止调用（见 IStorage.h）。未 prepare 时返回 nullptr
    const uint8_t *readOnlyMemPtr() const override { return isPrepared() ? _mem.data() : nullptr; }
    uint8_t *writableMemPtr() override { return isPrepared() ? _mem.data() : nullptr; }
    size_t memSize() const override { return _depth * ((_data_width + 7) / 8); }

    FIFO(const std::string &name, int data_width, int depth, bool has_rst = false);

    // 存储分配放在 prepare：构造只做参数校验和建引脚
    bool prepareImpl(Context &ctx, std::string &error) override;

    int dataWidth() const {
        return _data_width;
    }
    int fifoDepth() const {
        return _depth;
    }
    // 裸指针，仅供仿真框架，元件禁止调用。未 prepare 时返回 nullptr
    const uint8_t *data() const {
        return isPrepared() ? _mem.data() : nullptr;
    }
    uint8_t *data() {
        return isPrepared() ? _mem.data() : nullptr;
    }

    std::string genStructDef() const override {
        return "";
    }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    // 只在 prepare() 里调用
    void allocateMem();
    int _data_width, _depth;
    bool _has_rst;
    std::vector<uint8_t> _mem;
};

} // namespace dsc
