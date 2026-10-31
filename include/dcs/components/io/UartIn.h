// UartIn — 宿主 stdin → 8N1 串行比特流
//
// 与 UART 元件的接收端配对：用户在终端敲的字符经过它变成一串电平，
// 送到 UART 的 rx 引脚，内核的 8250 驱动就能收到。baud_div 必须和
// UART 元件一致，否则位宽对不上，收到的全是乱码。
//
// 字符来源支持两种：终端交互（_kbhit/_getch）和管道/重定向（ReadFile 非阻塞）。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class UartIn : public SequentialComponent {
public:
    explicit UartIn(const std::string &name, int baud_div = 16);
    ~UartIn() override;

    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 测试用：把字节塞进发送队列，不走 stdin
    void pushByte(uint8_t b);
    // 队列里还有多少字节没发
    size_t pending() const;

private:
    struct InState {
        uint32_t baud_div;
        uint8_t fifo[64];
        uint8_t head, tail;
        // 每次去问 stdin 都要一次系统调用，逐拍问会把仿真拖慢两三个数量级。
        // 空闲时 JIT 每 POLL_INTERVAL 拍才回调一次，按 tick 折算的延迟远小于串口一位
        static constexpr uint32_t POLL_INTERVAL = 1024;
        // "队列里可能有数据"的提示，JIT 每拍只读这一个字
        volatile uint32_t has_data;
    };
    InState _state{};
    int _baud_div = 16;

    static int fetch(void *state_ptr, int do_poll);
};

} // namespace dsc
