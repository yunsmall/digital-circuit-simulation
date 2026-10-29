// UART — 16550A 兼容的 8N1 串口（stride 4，对应设备树 reg-shift=2、reg-io-width=4）
//
// Wishbone 从设备，寄存器：
//   0x00 RBR(读)/THR(写)/DLL   LCR.DLAB=1 时访问 DLL
//   0x04 IER/DLM               LCR.DLAB=1 时访问 DLM
//   0x08 IIR(读)/FCR(写)
//   0x0C LCR
//   0x10 MCR
//   0x14 LSR                   bit0=DR（收到数据） bit5=THRE bit6=TEMT
//   0x18 MSR
//   0x1C SCR
//
// tx/rx 是串行线，按 baud_div 个 tick 一位输出/采样，与真实 UART 的时序关系一致，
// 只是这里的"时间单位"是仿真 tick。DLL/DLM 只保存不影响分频（分频由构造参数固定）。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class UART : public SequentialComponent {
public:
    explicit UART(const std::string &name, int baud_div = 8);
    ~UART() override;

    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    struct UartState {
        uint32_t baud_div;
        // 发送
        bool tx_busy;
        uint32_t tx_cnt;
        uint8_t tx_bit;    // 已发送的位数（0=起始位，1..8=数据，9=停止位）
        uint32_t tx_shift; // 待发送字节
        // 接收
        bool rx_active;
        bool rx_confirm; // 正在起始位中点做确认
        uint32_t rx_cnt;
        uint8_t rx_bit;
        uint32_t rx_shift;
        uint32_t rx_data;
        bool rx_valid;
        bool prev_rx;
        // 控制与总线
        uint32_t ier, lcr, mcr, fcr, scr;   // 16550 的配置寄存器
        uint32_t dll, dlm;                  // 波特率分频（只保存，分频由构造参数决定）
        uint32_t latched_dout;
        bool prev_stb;
        bool irq;                           // 有中断待处理
    };
    UartState _state;
    int _baud_div = 8;
    bool _prepared = false;

    static void uartTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs);
    static void uartReset(void *state_ptr, int baud_div);
};

} // namespace dsc
