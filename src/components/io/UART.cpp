// UART — 16550A 兼容串口
// 全部逻辑（发送/接收状态机、16 字节接收 FIFO、寄存器组）都展开在 JIT 里，
// 每拍没有跨模块调用。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/UART.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
// Wishbone 引脚索引
constexpr int I_ADR = 0, I_DAT = 1, I_WE = 2, I_SEL = 3, I_STB = 4, I_CYC = 5, I_CLK = 6, I_RX = 7;
constexpr int O_DAT = 0, O_ACK = 1, O_TX = 2, O_IRQ = 3;
constexpr uint32_t LCR_DLAB = 0x80;
} // namespace

UART::UART(const std::string &name, int baud_div) : SequentialComponent(name, "uart"), _baud_div(baud_div) {
    if (_baud_div < 2)
        _baud_div = 2;
    setParam("baud_div", std::to_string(_baud_div));

    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("clk", 1);
    addInput("rx", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("tx", 1, false, true);
    addOutput("irq", 1, false, true);
}

UART::~UART() = default;


std::string UART::genStateDecl() const {
    return std::format(
            "#define _UART_{0}_RBR 0x00u\n"
            "#define _UART_{0}_IER 0x04u\n"
            "#define _UART_{0}_IIR 0x08u\n"
            "#define _UART_{0}_LCR 0x0Cu\n"
            "#define _UART_{0}_MCR 0x10u\n"
            "#define _UART_{0}_LSR 0x14u\n"
            "#define _UART_{0}_MSR 0x18u\n"
            "#define _UART_{0}_SCR 0x1Cu\n"
            "#define _UART_{0}_DLAB 0x80u\n"
            "static uint32_t _uart_baud_{0};\n"
            "static uint8_t _uart_tx_busy_{0}, _uart_tx_bit_{0};\n"
            "static uint32_t _uart_tx_cnt_{0}, _uart_tx_shift_{0};\n"
            "static uint8_t _uart_rx_active_{0}, _uart_rx_confirm_{0}, _uart_rx_bit_{0}, _uart_prev_rx_{0};\n"
            "static uint32_t _uart_rx_cnt_{0}, _uart_rx_shift_{0};\n"
            "static uint8_t _uart_rx_fifo_{0}[16], _uart_rx_head_{0}, _uart_rx_tail_{0}, _uart_rx_count_{0};\n"
            "static uint8_t _uart_rx_overrun_{0}, _uart_prev_stb_{0};\n"
            "static uint32_t _uart_ier_{0}, _uart_lcr_{0}, _uart_mcr_{0}, _uart_fcr_{0}, _uart_scr_{0};\n"
            "static uint32_t _uart_dll_{0}, _uart_dlm_{0}, _uart_latched_{0};\n",
            _id);
}

std::string UART::genInitCode() const {
    std::string c = std::format(
            "    _uart_baud_{0} = {1};\n"
            "    _uart_tx_busy_{0} = 0; _uart_tx_bit_{0} = 0; _uart_tx_cnt_{0} = 0; _uart_tx_shift_{0} = 0;\n"
            "    _uart_rx_active_{0} = 0; _uart_rx_confirm_{0} = 0; _uart_rx_bit_{0} = 0; _uart_prev_rx_{0} = 1;\n"
            "    _uart_rx_cnt_{0} = 0; _uart_rx_shift_{0} = 0;\n"
            "    _uart_rx_head_{0} = 0; _uart_rx_tail_{0} = 0; _uart_rx_count_{0} = 0; _uart_rx_overrun_{0} = 0;\n"
            "    _uart_prev_stb_{0} = 0;\n"
            "    _uart_ier_{0} = 0; _uart_lcr_{0} = 0x03; _uart_mcr_{0} = 0; _uart_fcr_{0} = 0; _uart_scr_{0} = 0;\n"
            "    _uart_dll_{0} = 0; _uart_dlm_{0} = 0; _uart_latched_{0} = 0;\n",
            _id, _baud_div);
    // 串口空闲时 TX 是高电平。空闲快速路径会跳过输出刷新，复位后的第一拍必须
    // 已经把线拉高，否则会一直是低（低电平是起始位）
    const int tx_nid = outputs()[O_TX]->netId();
    if (tx_nid >= 0)
        c += std::format("    _w[{0}][0] = 1; dcs_memset(_w[{0}] + 1, 0, 15);\n", tx_nid);
    return c;
}

std::string UART::genFuncDef_seq() const {
    // 悬空输入必须读到 0：直接写 _w[-1] 会越界，测试里偶尔会踩到不可读页
    auto rd = [&](int pin, const char *var, int width) {
        const Pin *p = inputs()[pin].get();
        const int nw = p->net() ? p->net()->bit_width() : 0;
        return gen_read_wire(p->netId(), width, nw, var);
    };
    std::string c;
    c += std::format("// UART {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    " + rd(I_STB, "_stb", 1) + "\n";
    c += "    " + rd(I_CYC, "_cyc", 1) + "\n";
    c += "    " + rd(I_RX, "_rx", 1) + "\n";
    c += std::format(
            "    const bool _active = _stb && _cyc;\n"
            // 空闲快速路径：总线没选中、收发状态机都在空闲、接收线也没变化时，
            // 本拍不会推进任何状态，读取地址/数据、寄存器译码和 4 根输出线全跳过
            "    if (!_active && !_uart_prev_stb_{0} && !_uart_tx_busy_{0} && !_uart_rx_active_{0} && "
            "_rx == _uart_prev_rx_{0}) return;\n"
            "    const bool _rise = _active && !_uart_prev_stb_{0};\n"
            "    _uart_prev_stb_{0} = _active;\n",
            _id);
    c += "    " + rd(I_ADR, "_adr", 32) + "\n";
    c += "    " + rd(I_DAT, "_wdat", 32) + "\n";
    c += "    " + rd(I_WE, "_we", 1) + "\n";
    c += "    uint32_t _dout = 0; uint8_t _ack = 0;\n";
    // 发送状态机
    c += std::format(
            "    if (_uart_tx_busy_{0}) {{\n"
            "        if (++_uart_tx_cnt_{0} >= _uart_baud_{0}) {{\n"
            "            _uart_tx_cnt_{0} = 0;\n"
            "            if (++_uart_tx_bit_{0} > 9) _uart_tx_busy_{0} = 0;\n"
            "        }}\n"
            "    }}\n",
            _id);
    // 接收状态机
    c += std::format(
            "    if (!_uart_rx_active_{0}) {{\n"
            "        if (_uart_prev_rx_{0} && !_rx) {{\n"
            "            _uart_rx_active_{0} = 1; _uart_rx_confirm_{0} = 1;\n"
            "            _uart_rx_cnt_{0} = 0; _uart_rx_bit_{0} = 0; _uart_rx_shift_{0} = 0;\n"
            "        }}\n"
            "    }} else if (++_uart_rx_cnt_{0} >= (_uart_rx_confirm_{0} ? (_uart_baud_{0} / 2) : _uart_baud_{0})) {{\n"
            "        _uart_rx_cnt_{0} = 0;\n"
            "        if (_uart_rx_confirm_{0}) {{\n"
            "            _uart_rx_confirm_{0} = 0;\n"
            "            if (_rx) _uart_rx_active_{0} = 0;\n"
            "            else _uart_rx_cnt_{0} = 0;\n"
            "        }} else if (_uart_rx_bit_{0} < 8) {{\n"
            "            _uart_rx_shift_{0} |= (uint32_t)(_rx ? 1u : 0u) << _uart_rx_bit_{0};\n"
            "            _uart_rx_bit_{0}++;\n"
            "        }} else {{\n"
            "            _uart_rx_active_{0} = 0;\n"
            "            if (_rx) {{\n"
            "                if (_uart_rx_count_{0} < 16) {{\n"
            "                    _uart_rx_fifo_{0}[_uart_rx_head_{0} & 15] = (uint8_t)(_uart_rx_shift_{0} & 0xFFu);\n"
            "                    _uart_rx_head_{0}++; _uart_rx_count_{0}++;\n"
            "                }} else _uart_rx_overrun_{0} = 1;\n"
            "            }}\n"
            "        }}\n"
            "    }}\n"
            "    _uart_prev_rx_{0} = _rx;\n",
            _id);
    // 总线读
    c += std::format(
            "    const bool _dlab = (_uart_lcr_{0} & _UART_{0}_DLAB) != 0;\n"
            "    const uint32_t _off = (_adr & 0x1Fu) & ~3u;\n"
            "    if (_rise) {{\n"
            "        if (!_we) {{\n"
            "            switch (_off) {{\n"
            "            case _UART_{0}_RBR:\n"
            "                if (_dlab) _dout = _uart_dll_{0};\n"
            "                else if (_uart_rx_count_{0}) {{\n"
            "                    _dout = _uart_rx_fifo_{0}[_uart_rx_tail_{0} & 15];\n"
            "                    _uart_rx_tail_{0}++; _uart_rx_count_{0}--;\n"
            "                }}\n"
            "                break;\n"
            "            case _UART_{0}_IER: _dout = _dlab ? _uart_dlm_{0} : _uart_ier_{0}; break;\n"
            "            case _UART_{0}_IIR: {{\n"
            "                uint32_t _iir = 0x1;\n"
            "                if (_uart_rx_count_{0} && (_uart_ier_{0} & 0x01)) _iir = 0x4;\n"
            "                else if (!_uart_tx_busy_{0} && (_uart_ier_{0} & 0x02)) _iir = 0x2;\n"
            // 不报 FIFO 位：TX 只有单字节寄存器，声称有 16 字节 FIFO 会让内核一次
            // 连写 16 个字符，只有最后一个留下来
            "                _dout = _iir; break;\n"
            "            }}\n"
            "            case _UART_{0}_LCR: _dout = _uart_lcr_{0}; break;\n"
            "            case _UART_{0}_MCR: _dout = _uart_mcr_{0}; break;\n"
            "            case _UART_{0}_LSR:\n"
            "                _dout = (_uart_rx_count_{0} ? 0x01u : 0u) | (_uart_rx_overrun_{0} ? 0x02u : 0u) |\n"
            "                        (!_uart_tx_busy_{0} ? 0x60u : 0u);\n"
            "                _uart_rx_overrun_{0} = 0;\n"
            "                break;\n"
            "            case _UART_{0}_MSR: _dout = 0x30; break;\n"
            "            case _UART_{0}_SCR: _dout = _uart_scr_{0}; break;\n"
            "            default: _dout = 0; break;\n"
            "            }}\n"
            "        }} else {{\n"
            "            switch (_off) {{\n"
            "            case _UART_{0}_RBR:\n"
            "                if (_dlab) _uart_dll_{0} = _wdat & 0xFFu;\n"
            "                else if (!_uart_tx_busy_{0}) {{\n"
            "                    _uart_tx_shift_{0} = _wdat & 0xFFu;\n"
            "                    _uart_tx_busy_{0} = 1; _uart_tx_bit_{0} = 0; _uart_tx_cnt_{0} = 0;\n"
            "                }}\n"
            "                break;\n"
            "            case _UART_{0}_IER: if (_dlab) _uart_dlm_{0} = _wdat & 0xFFu; else _uart_ier_{0} = _wdat & 0x0Fu; break;\n"
            "            case _UART_{0}_IIR: _uart_fcr_{0} = _wdat & 0xC9u; break;\n"
            "            case _UART_{0}_LCR: _uart_lcr_{0} = _wdat & 0xFFu; break;\n"
            "            case _UART_{0}_MCR: _uart_mcr_{0} = _wdat & 0x1Fu; break;\n"
            "            case _UART_{0}_SCR: _uart_scr_{0} = _wdat & 0xFFu; break;\n"
            "            default: break;\n"
            "            }}\n"
            "        }}\n"
            "        _ack = 1;\n"
            "        _uart_latched_{0} = _dout;\n"
            "    }} else if (_active) {{\n"
            "        _dout = _uart_latched_{0};\n"
            "        _ack = 1;\n"
            "    }}\n"
            "    if (!_active) _uart_latched_{0} = 0;\n"
            "    uint8_t _irq = (_uart_rx_count_{0} && (_uart_ier_{0} & 0x01)) ||\n"
            "                    (!_uart_tx_busy_{0} && (_uart_ier_{0} & 0x02));\n"
            "    uint32_t _txl = 1;\n"
            "    if (_uart_tx_busy_{0}) {{\n"
            "        if (_uart_tx_bit_{0} == 0) _txl = 0;\n"
            "        else if (_uart_tx_bit_{0} <= 8) _txl = (_uart_tx_shift_{0} >> (_uart_tx_bit_{0} - 1)) & 1u;\n"
            "        else _txl = 1;\n"
            "    }}\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_TX, "_txl", 1) + "\n";
    c += "    " + genOutputWrite(O_IRQ, "_irq", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> UART::clone(const std::string &new_name) const {
    return std::make_unique<UART>(new_name, _baud_div);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"uart", {{"baud_div", "8"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "rx"}, {"wb_dat_o", "wb_ack_o", "tx", "irq"}, dsc::ComponentCategory::Misc, "16550A 兼容 8N1 串口（reg-shift=2），按 baud_div 个 tick 一位"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UART>(n, dsc::paramInt(p, "baud_div", 8));
        });
} // namespace
