#include "dcs/components/UART.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_RX, I_COUNT };
enum OutIdx { O_DAT = 0, O_ACK, O_TX, O_COUNT };

constexpr uint32_t REG_TXDATA = 0x00;
constexpr uint32_t REG_RXDATA = 0x04;
constexpr uint32_t REG_STATUS = 0x08;
constexpr uint32_t REG_CONTROL = 0x0C;
} // namespace

UART::UART(const std::string &name, int baud_div) : SequentialComponent(name, "uart"), _baud_div(baud_div) {
    if (_baud_div < 2)
        _baud_div = 2;
    setParam("baud_div", std::to_string(_baud_div));
    addInput("clk", 1);
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("rx", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("tx", 1, false, true);
}

UART::~UART() = default;

bool UART::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> UART::extraJitSymbols() const {
    return {{"_dcs_uart_tick", reinterpret_cast<void *>(&UART::uartTick)},
            {"_dcs_uart_reset", reinterpret_cast<void *>(&UART::uartReset)}};
}

void UART::uartReset(void *state_ptr, int baud_div) {
    auto &s = *static_cast<UartState *>(state_ptr);
    std::memset(&s, 0, sizeof(s));
    // 半位 = baud_div/2，取 1 时半位为 0，接收状态机会每 tick 推进一次
    s.baud_div = (uint32_t) (baud_div < 2 ? 2 : baud_div);
    s.control = 0x3; // 默认收发都使能
}

std::string UART::genStateDecl() const {
    return std::format("static void *_uart_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string UART::genInitCode() const {
    return std::format("    _dcs_uart_reset(_uart_state_{}, {});\n", _id, _baud_div);
}

std::string UART::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_uart_tick(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_uart_reset(void *, int);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += genInputPtrArray("_in");
    // 输出先落中间缓冲，再由 genOutputWrite 同时写 _c_out 与 _w；
    // 传给回调的必须是指针数组，二维数组强转会让回调把首字节当指针
    code += std::format("    uint8_t _obuf[{}][{}];\n", (int) O_COUNT, SLOT);
    code += "    dcs_memset(_obuf, 0, sizeof(_obuf));\n";
    code += std::format("    uint8_t *_op[{}] = {{", (int) O_COUNT);
    for (int i = 0; i < O_COUNT; i++) code += std::format("{}_obuf[{}]", i ? "," : "", i);
    code += "};\n";
    code += std::format("    _dcs_uart_tick(_uart_state_{0}, (const uint8_t **)_in, _op);\n", _id);
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_obuf[0],4); " + genOutputWrite(O_DAT, "_d", 32) + " }\n";
    code += "    { uint8_t _a=0; dcs_memcpy(&_a,_obuf[1],1); " + genOutputWrite(O_ACK, "_a", 1) + " }\n";
    code += "    { uint8_t _t=0; dcs_memcpy(&_t,_obuf[2],1); " + genOutputWrite(O_TX, "_t", 1) + " }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> UART::clone(const std::string &new_name) const {
    auto u = std::make_unique<UART>(new_name, _baud_div);
    u->_state = _state;
    return u;
}

void UART::uartTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &s = *static_cast<UartState *>(state_ptr);

    uint32_t adr = 0, wdat = 0;
    std::memcpy(&adr, inputs[I_ADR], 4);
    std::memcpy(&wdat, inputs[I_DAT], 4);
    bool we = (inputs[I_WE][0] & 1) != 0;
    bool stb = (inputs[I_STB][0] & 1) != 0;
    bool cyc = (inputs[I_CYC][0] & 1) != 0;
    bool rx = (inputs[I_RX][0] & 1) != 0;

    bool active = stb && cyc;
    bool stb_rise = active && !s.prev_stb;
    s.prev_stb = active;

    // ---- 发送：起始位 0 -> 8 位数据（LSB 先）-> 停止位 1 ----
    if (s.tx_busy) {
        if (++s.tx_cnt >= s.baud_div) {
            s.tx_cnt = 0;
            if (++s.tx_bit > 9)
                s.tx_busy = false;
        }
    }

    // ---- 接收：起始沿 -> 半位确认起始位 -> 每位中点采样 -> 停止位校验 ----
    // 半位确认可以把窄脉冲噪声挡掉，中点采样则让波特率有偏差时仍落在位中央
    if (!s.rx_active) {
        if (s.prev_rx && !rx && (s.control & 2u)) { // 空闲高转低 = 起始位，且 RX 已使能
            s.rx_active = true;
            s.rx_confirm = true;
            s.rx_cnt = 0;
            s.rx_bit = 0;
            s.rx_shift = 0;
        }
    } else if (++s.rx_cnt >= (s.rx_confirm ? s.baud_div / 2 : s.baud_div)) {
        s.rx_cnt = 0;
        if (s.rx_confirm) {
            s.rx_confirm = false;
            if (rx) {
                s.rx_active = false; // 半位处已回高，是噪声
            } else {
                s.rx_cnt = 0; // 再走满一位，落点才是 bit0 中点
            }
        } else if (s.rx_bit < 8) {
            s.rx_shift |= (uint32_t) (rx ? 1 : 0) << s.rx_bit;
            s.rx_bit++;
        } else {
            s.rx_active = false;
            if (rx) { // 停止位必须是高，否则算帧错误，不交付数据
                s.rx_data = s.rx_shift & 0xFFu;
                s.rx_valid = true;
            }
        }
    }
    s.prev_rx = rx;

    // ---- 总线访问 ----
    uint32_t dout = 0;
    bool ack = false;
    if (stb_rise) {
        const uint32_t off = adr & 0x0Fu;
        if (!we) {
            switch (off) {
            case REG_RXDATA:
                dout = s.rx_data;
                s.rx_valid = false; // 读走即清
                break;
            case REG_STATUS:
                dout = (s.tx_busy ? 0u : 1u) | (s.rx_valid ? 2u : 0u);
                break;
            case REG_CONTROL: dout = s.control; break;
            default: dout = 0; break;
            }
        } else {
            switch (off) {
            case REG_TXDATA:
                if (!s.tx_busy && (s.control & 1u)) { // TX 使能且空闲才接受
                    s.tx_shift = wdat & 0xFFu;
                    s.tx_busy = true;
                    s.tx_bit = 0;
                    s.tx_cnt = 0;
                }
                break;
            case REG_CONTROL: s.control = wdat & 0x3u; break;
            default: break;
            }
        }
        ack = true;
        s.latched_dout = dout;
    } else if (active) {
        dout = s.latched_dout; // 事务期间保持应答数据
        ack = true;
    }
    if (!active)
        s.latched_dout = 0;

    // 串行输出：起始位为 0，数据位从低到高，停止位为 1
    uint32_t tx_level = 1;
    if (s.tx_busy && (s.control & 1u)) {
        if (s.tx_bit == 0)
            tx_level = 0;
        else if (s.tx_bit <= 8)
            tx_level = (s.tx_shift >> (s.tx_bit - 1)) & 1u;
        else
            tx_level = 1;
    }

    std::memset(outputs[O_DAT], 0, SLOT);
    std::memcpy(outputs[O_DAT], &dout, 4);
    outputs[O_ACK][0] = ack ? 1 : 0;
    outputs[O_TX][0] = (uint8_t) (tx_level & 1u);
}

} // namespace dsc
