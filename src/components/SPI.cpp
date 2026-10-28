#include "dcs/components/SPI.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_MISO, I_COUNT };
enum OutIdx { O_DAT = 0, O_ACK, O_SCLK, O_MOSI, O_CS, O_COUNT };

constexpr uint32_t REG_TXDATA = 0x00;
constexpr uint32_t REG_RXDATA = 0x04;
constexpr uint32_t REG_STATUS = 0x08;
constexpr uint32_t REG_CONTROL = 0x0C;
constexpr uint32_t REG_CLKDIV = 0x10;
} // namespace

SPI::SPI(const std::string &name, int clkdiv) : SequentialComponent(name, "spi"), _clkdiv(clkdiv) {
    if (_clkdiv < 1)
        _clkdiv = 1;
    setParam("clkdiv", std::to_string(_clkdiv));
    addInput("clk", 1);
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("miso", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("sclk", 1, false, true);
    addOutput("mosi", 1, false, true);
    addOutput("cs", 1, false, true);
}

SPI::~SPI() = default;

bool SPI::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> SPI::extraJitSymbols() const {
    return {{"_dcs_spi_tick", reinterpret_cast<void *>(&SPI::spiTick)},
            {"_dcs_spi_reset", reinterpret_cast<void *>(&SPI::spiReset)}};
}

void SPI::spiReset(void *state_ptr, int clkdiv) {
    auto &s = *static_cast<SpiState *>(state_ptr);
    std::memset(&s, 0, sizeof(s));
    s.clkdiv = (uint32_t) (clkdiv < 1 ? 1 : clkdiv);
    s.sclk = false;
}

std::string SPI::genStateDecl() const {
    return std::format("static void *_spi_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string SPI::genInitCode() const { return std::format("    _dcs_spi_reset(_spi_state_{}, {});\n", _id, _clkdiv); }

std::string SPI::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_spi_tick(void *, const uint8_t *, uint8_t *);\n";
    code += "extern void _dcs_spi_reset(void *, int);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += std::format("    uint8_t _in[{}][{}];\n", (int) I_COUNT, SLOT);
    for (int i = 0; i < I_COUNT; i++) {
        int w = (i == I_ADR || i == I_DAT) ? 32 : (i == I_SEL ? 4 : 1);
        code += std::format("    {{ uint64_t _v=0; dcs_memcpy(&_v,_w[{}],{}); uint8_t _b[8]; dcs_memcpy(_b,&_v,8); "
                            "dcs_memcpy(_in[{}],_b,{}); }}\n",
                            inputs()[i]->netId(), byte_count(w), i, byte_count(w));
    }
    code += std::format("    uint8_t _out[{}][{}];\n", (int) O_COUNT, SLOT);
    code += std::format("    _dcs_spi_tick(_spi_state_{0}, (const uint8_t*)_in, (uint8_t*)_out);\n", _id);
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_out[0],4); " + genOutputWrite(O_DAT, "_d", 32) + " }\n";
    code += "    { uint8_t _a=0; dcs_memcpy(&_a,_out[1],1); " + genOutputWrite(O_ACK, "_a", 1) + " }\n";
    code += "    { uint8_t _s=0; dcs_memcpy(&_s,_out[2],1); " + genOutputWrite(O_SCLK, "_s", 1) + " }\n";
    code += "    { uint8_t _m=0; dcs_memcpy(&_m,_out[3],1); " + genOutputWrite(O_MOSI, "_m", 1) + " }\n";
    code += "    { uint8_t _c=0; dcs_memcpy(&_c,_out[4],1); " + genOutputWrite(O_CS, "_c", 1) + " }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> SPI::clone(const std::string &new_name) const {
    auto p = std::make_unique<SPI>(new_name, _clkdiv);
    p->_state = _state;
    return p;
}

void SPI::spiTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs) {
    auto &s = *static_cast<SpiState *>(state_ptr);

    uint32_t adr = 0, wdat = 0;
    std::memcpy(&adr, inputs + I_ADR * SLOT, 4);
    std::memcpy(&wdat, inputs + I_DAT * SLOT, 4);
    bool we = (inputs[I_WE * SLOT] & 1) != 0;
    bool stb = (inputs[I_STB * SLOT] & 1) != 0;
    bool cyc = (inputs[I_CYC * SLOT] & 1) != 0;
    bool miso = (inputs[I_MISO * SLOT] & 1) != 0;

    bool active = stb && cyc;
    bool stb_rise = active && !s.prev_stb;
    s.prev_stb = active;

    // ---- 位时序：每半个位周期翻转 sclk ----
    if (s.busy) {
        if (++s.div_cnt >= s.clkdiv) {
            s.div_cnt = 0;
            s.sclk = !s.sclk;
            if (!s.sclk) {
                // 下降沿移出下一位（从高到低依次发送）
                if (s.bits_left > 0) {
                    s.bits_left--;
                    if (s.bits_left == 0) {
                        s.busy = false;
                        s.done = true;
                        s.rx_data = s.rx_shift & 0xFFu;
                    }
                }
            } else {
                // 上升沿采样
                s.rx_shift = (s.rx_shift << 1) | (miso ? 1u : 0u);
            }
        }
    }

    // ---- 总线访问 ----
    uint32_t dout = 0;
    bool ack = false;
    if (stb_rise) {
        const uint32_t off = adr & 0x1Fu;
        if (!we) {
            switch (off) {
            case REG_RXDATA:
                dout = s.rx_data;
                s.done = false;
                break;
            case REG_STATUS: dout = (s.busy ? 1u : 0u) | (s.done ? 2u : 0u); break;
            case REG_CLKDIV: dout = s.clkdiv; break;
            case REG_CONTROL: dout = s.control; break;
            default: dout = 0; break;
            }
        } else {
            switch (off) {
            case REG_TXDATA: s.tx_shift = wdat & 0xFFu; break;
            case REG_CLKDIV:
                if (!s.busy && (wdat & 0xFFFFu))
                    s.clkdiv = wdat & 0xFFFFu;
                break;
            case REG_CONTROL:
                s.control = wdat;
                if ((wdat & 1u) && !s.busy) { // 启动一次传输
                    s.busy = true;
                    s.done = false;
                    s.bits_left = 8;
                    s.rx_shift = 0;
                    s.div_cnt = 0;
                    s.sclk = false;
                }
                break;
            default: break;
            }
        }
        ack = true;
        s.latched_dout = dout;
    } else if (active) {
        dout = s.latched_dout;
        ack = true;
    }
    if (!active)
        s.latched_dout = 0;

    // 模式 0：空闲低，传输期间 cs 拉低，发送最高位
    uint32_t mosi = s.busy ? ((s.tx_shift >> (s.bits_left ? s.bits_left - 1 : 7)) & 1u) : 0u;
    uint32_t cs = s.busy ? 0u : 1u;

    std::memset(outputs, 0, O_COUNT * SLOT);
    std::memcpy(outputs + O_DAT * SLOT, &dout, 4);
    outputs[O_ACK * SLOT] = ack ? 1 : 0;
    outputs[O_SCLK * SLOT] = s.sclk ? 1 : 0;
    outputs[O_MOSI * SLOT] = (uint8_t) mosi;
    outputs[O_CS * SLOT] = (uint8_t) cs;
}

} // namespace dsc
