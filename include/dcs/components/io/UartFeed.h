// UartFeed — 由外部喂数据的 8N1 串行发送器
//
// 与 UartIn 的区别：它不碰 stdin，不做任何系统调用。外部程序（CLI 的读线程、
// 测试代码）通过 push() 把字节塞进内部队列，元件按 baud_div 逐拍把队列里的
// 字节编成串行电平送到 UART 的 rx 引脚。
//
// 这样仿真热路径上只有一次队列探测，不会被输入检测拖慢——UartIn 每拍一次
// _kbhit/PeekNamedPipe 曾经把仿真速度拉低了两三个数量级。
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class UartFeed : public SequentialComponent {
public:
    explicit UartFeed(const std::string &name, int baud_div = 16);
    ~UartFeed() override;

    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 生产者侧（可以是别的线程）：队列满返回 false，由调用方决定重试还是丢弃
    bool push(uint8_t b);
    size_t push(const uint8_t *data, size_t len);
    // 队列里还有多少字节没发出去（测试用）
    size_t pending() const;

private:
    static constexpr uint32_t CAP = 4096;

    struct FeedState {
        uint32_t baud_div;
        uint32_t cnt;
        uint8_t bit;    // 0=起始位，1..8=数据位（LSB 先），9=停止位
        bool busy;
        uint8_t ch;
        // 单生产者单消费者环形队列：head 只由生产者写，tail 只由消费者写。
        // JIT 每拍用 dcs_load_acq 比较两者，非空才回调；不再另设"有数据"标志——
        // 标志的"判空后清零"会被并发的 push 覆盖，导致字节永久卡在队列里。
        // 字段保持普通 uint32_t：JIT 用 dcs_load_acq 读同一块存储，宿主侧用
        // atomic_ref 做原子访问。声明成 std::atomic 再把地址交给 JIT，等于通过
        // 非 atomic 指针访问 atomic 对象，标准上属于 UB（和 UartIn 一致）
        uint32_t head, tail;
        uint8_t buf[CAP];
    };
    FeedState _state{};
    int _baud_div = 16;
    // head 只由生产者写，但 --forever --watch 下 stdin 线程和 watch 主循环
    // 会同时 push；串行化生产者，消费者（JIT）仍然只读原子 head/tail
    std::mutex _push_mutex;

    static int feedPop(void *state_ptr);
};

} // namespace dsc
