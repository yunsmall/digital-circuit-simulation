// UartIn — 宿主字节流 → 8N1 串行比特流
//
// 与 UART 元件的接收端配对：宿主给的字符经过它变成一串电平，
// 送到 UART 的 rx 引脚，内核的 8250 驱动就能收到。baud_div 必须和
// UART 元件一致，否则位宽对不上，收到的全是乱码。
//
// 字节来源支持三种，互不干扰、都是实例级的：
//   1. pushByte()：外部线程直接塞队列，适合 GUI 按键这种即时输入；
//   2. setSource()：绑定自定义轮询源，适合 GUI/脚本按自己的方式取字符；
//   3. 未绑定时回退到终端（_kbhit/_getch）或管道/重定向（ReadFile 非阻塞）。
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class UartIn : public SequentialComponent {
public:
    explicit UartIn(const std::string &name, int baud_div = 8);
    ~UartIn() override;

    std::vector<JitSymbol> extraJitSymbols() const override;
    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    // 只复位发送状态机。宿主输入队列不清空：队列属于外部世界，里面的字节相当于
    // 已经排在线路上的输入，复位时丢掉会吞掉用户已经敲进去的内容
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 把字节塞进发送队列，不走 stdin。允许和 tick 在别的线程并发调用，
    // 但这个函数是单生产者实现：同一实例只能有一个线程调用它（多个线程要先自己串行化）
    void pushByte(uint8_t b);
    // 队列里还有多少字节没发
    size_t pending() const;

    // 实例级输入源：没有队列数据时，按 POLL_INTERVAL 的节奏调用 poll()。
    // 返回 0~255 表示有字节，-1 表示暂时没有。回调在仿真线程执行，
    // 必须在开始 tick 之前设置；传空 Source 即恢复默认终端/管道输入
    struct Source {
        int (*poll)(void *ctx, UartIn *owner) = nullptr;
    };
    void setSource(const Source &source, void *ctx);
    void clearSource() { setSource({}, nullptr); }

private:
    struct InState {
        uint32_t baud_div;
        uint8_t fifo[64];
        // 生产者（pushByte）和消费者（JIT 回调 fetch）可能在不同线程。
        // 字段故意保持普通 uint32_t：JIT 用 dcs_load_acq 读同一块存储，宿主侧用
        // atomic_ref 做原子访问。声明成 std::atomic 再把地址交给 JIT，等于通过
        // 非 atomic 指针访问 atomic 对象，标准上属于 UB
        uint32_t head, tail;
        // 每次去问 stdin 都要一次系统调用，逐拍问会把仿真拖慢两三个数量级。
        // 空闲时 JIT 每 POLL_INTERVAL 拍才回调一次，按 tick 折算的延迟远小于串口一位
        static constexpr uint32_t POLL_INTERVAL = 1024;
    };
    InState _state{};
    int _baud_div = 8;
    Source _source{};
    void *_source_ctx = nullptr;

    // JIT 回调：先取队列，队列空且允许轮询时才去问输入源
    static int fetch(void *owner, void *state_ptr, int do_poll);
    int pollSource(int do_poll);
};

} // namespace dsc
