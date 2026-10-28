// AxiLiteBridge — AXI4-Lite 从设备 → Wishbone 主设备
//
// 让 AXI4-Lite 主设备（如 Zynq 的 PS 端口、各类 AXI IP）访问本项目的 Wishbone 总线。
// 写通道 AW/W 各自独立握手，收齐地址和数据后发一次 Wishbone 写，B 通道回响应；
// 读通道 AR 收到即发一次 Wishbone 读，拿到数据后在 R 通道返回。
// 同一时刻只处理一笔事务（无 outstanding），足够应付寄存器访问。
#pragma once
#include <cstdint>
#include <string>
#include "dcs/Component.h"

namespace dsc {

class AxiLiteBridge : public SequentialComponent {
public:
    explicit AxiLiteBridge(const std::string &name);
    ~AxiLiteBridge() override;

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
    enum class St : uint8_t { IDLE, WRITE_GO, WRITE_WAIT, WRITE_RESP, READ_GO, READ_WAIT, READ_RESP };

    struct BridgeState {
        St st;
        bool aw_got, w_got;
        uint32_t aw_addr, w_data;
        uint8_t w_strb;
        uint32_t r_data;
        // Wishbone 主侧
        uint32_t wb_adr, wb_dat;
        bool wb_we, wb_stb, wb_cyc;
        // AXI 从侧应答
        bool bvalid, rvalid;
        uint8_t bresp, rresp;
    };
    BridgeState _state;
    bool _prepared = false;

    static void bridgeTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs);
    static void bridgeReset(void *state_ptr);
};

} // namespace dsc
