// SPI — 8 位主机，模式 0（空闲低、上升沿采样）
//
// Wishbone 从设备，寄存器：
//   0x00 TXDATA  待发送字节
//   0x04 RXDATA  收到的字节
//   0x08 STATUS  bit0=忙, bit1=一次传输完成
//   0x0C CONTROL bit0=写 1 启动一次传输
//   0x10 CLKDIV  每半个位周期的 tick 数
//
// cs 低有效，sclk 由 clkdiv 分频产生；发送在 sclk 下降沿移出，接收在上升沿采样。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class SPI : public SequentialComponent {
public:
    explicit SPI(const std::string &name, int clkdiv = 4);
    ~SPI() override;

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
    struct SpiState {
        uint32_t clkdiv;
        bool busy;
        uint32_t div_cnt;
        bool sclk;
        uint8_t bits_left;
        uint32_t tx_shift;
        uint32_t rx_shift;
        uint32_t rx_data;
        bool done;
        uint32_t control;
        uint32_t latched_dout;
        bool prev_stb;
    };
    SpiState _state;
    int _clkdiv = 4;
    bool _prepared = false;

    static void spiTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs);
    static void spiReset(void *state_ptr, int clkdiv);
};

} // namespace dsc
