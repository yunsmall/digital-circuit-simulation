#include "dcs/components/I2C.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_SDA_IN };
enum OutIdx { O_DAT = 0, O_ACK, O_SCL, O_SDA_OUT, O_SDA_OE, O_COUNT };

constexpr uint32_t REG_TXDATA = 0x00;
constexpr uint32_t REG_RXDATA = 0x04;
constexpr uint32_t REG_STATUS = 0x08;
constexpr uint32_t REG_CONTROL = 0x0C;
constexpr uint32_t REG_ADDR = 0x10;
constexpr uint32_t REG_CLKDIV = 0x14;
} // namespace

I2C::I2C(const std::string &name, int clkdiv) : SequentialComponent(name, "i2c"), _clkdiv(clkdiv) {
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
    addInput("sda_in", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("scl", 1, false, true);
    addOutput("sda_out", 1, false, true);
    addOutput("sda_oe", 1, false, true);
}

I2C::~I2C() = default;

bool I2C::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> I2C::extraJitSymbols() const {
    return {{"_dcs_i2c_tick", reinterpret_cast<void *>(&I2C::i2cTick)},
            {"_dcs_i2c_reset", reinterpret_cast<void *>(&I2C::i2cReset)}};
}

void I2C::i2cReset(void *state_ptr, int clkdiv) {
    auto &s = *static_cast<I2cState *>(state_ptr);
    std::memset(&s, 0, sizeof(s));
    s.clkdiv = (uint32_t) (clkdiv < 1 ? 1 : clkdiv);
    s.phase = Phase::IDLE;
    s.scl = true; // I2C 总线空闲时 SCL 与 SDA 都被上拉为高
}

std::string I2C::genStateDecl() const {
    return std::format("static void *_i2c_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string I2C::genInitCode() const { return std::format("    _dcs_i2c_reset(_i2c_state_{}, {});\n", _id, _clkdiv); }

std::string I2C::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_i2c_tick(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_i2c_reset(void *, int);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += genInputPtrArray("_in");
    // 输出先落中间缓冲，再由 genOutputWrite 同时写 _c_out 与 _w；
    // 传给回调的必须是指针数组，二维数组强转会让回调把首字节当指针
    code += genOutputPtrArray((int) O_COUNT);
    code += std::format("    _dcs_i2c_tick(_i2c_state_{0}, _in, _op);\n", _id);
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_obuf[0],4); " + genOutputWrite(O_DAT, "_d", 32) + " }\n";
    code += "    { uint8_t _a=0; dcs_memcpy(&_a,_obuf[1],1); " + genOutputWrite(O_ACK, "_a", 1) + " }\n";
    code += "    { uint8_t _s=0; dcs_memcpy(&_s,_obuf[2],1); " + genOutputWrite(O_SCL, "_s", 1) + " }\n";
    code += "    { uint8_t _o=0; dcs_memcpy(&_o,_obuf[3],1); " + genOutputWrite(O_SDA_OUT, "_o", 1) + " }\n";
    code += "    { uint8_t _e=0; dcs_memcpy(&_e,_obuf[4],1); " + genOutputWrite(O_SDA_OE, "_e", 1) + " }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> I2C::clone(const std::string &new_name) const {
    auto p = std::make_unique<I2C>(new_name, _clkdiv);
    p->_state = _state;
    return p;
}

void I2C::i2cTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &s = *static_cast<I2cState *>(state_ptr);

    uint32_t adr = 0, wdat = 0;
    std::memcpy(&adr, inputs[I_ADR], 4);
    std::memcpy(&wdat, inputs[I_DAT], 4);
    bool we = (inputs[I_WE][0] & 1) != 0;
    bool stb = (inputs[I_STB][0] & 1) != 0;
    bool cyc = (inputs[I_CYC][0] & 1) != 0;
    bool sda_in = (inputs[I_SDA_IN][0] & 1) != 0;

    bool active = stb && cyc;
    bool stb_rise = active && !s.prev_stb;
    s.prev_stb = active;

    // ACK 位要在 SCL 高电平期间采样。在下降沿才读的话，纯组合的从机会在同一拍
    // 看到 SCL 变低就释放 SDA，主机读到的是上拉的高电平，把 ACK 误判成 NACK。
    // 采样点取 SCL 高电平的中间一拍。
    if (s.scl && !s.ack_sampled && (s.phase == Phase::ADDR_ACK || s.phase == Phase::DATA_ACK)
        && s.div_cnt >= s.clkdiv / 2) {
        s.ack_value = sda_in;
        s.ack_sampled = true;
    }

    // ---- 事务推进：每 clkdiv 个 tick 走一个 SCL 半周期 ----
    if (s.phase != Phase::IDLE && s.phase != Phase::DONE) {
        if (++s.div_cnt >= s.clkdiv) {
            s.div_cnt = 0;
            s.scl = !s.scl;
            if (s.scl) {
                // 停止条件要求 SCL 高电平期间 SDA 由低变高，只能在这里释放
                if (s.phase == Phase::STOP) {
                    s.sda_low = false;
                    s.phase = Phase::DONE;
                    s.done = true;
                } else if (s.phase == Phase::ADDR_ACK || s.phase == Phase::DATA_ACK) {
                    s.sda_low = false;       // 主机释放 SDA，从机此时才能拉低表示 ACK
                    s.ack_sampled = false;   // 这个 ACK 位还没采样
                }
            } else {
                // SCL 下降沿推进一位
                switch (s.phase) {
                case Phase::START:
                    // 起始条件已在写 CONTROL 时建立（SCL 高、SDA 低），
                    // 这里处在 SCL 下降沿，直接送出地址最高位，不能再动 scl
                    s.phase = Phase::ADDR;
                    s.bit_idx = 0;
                    s.shift = (s.addr & 0x7Fu) << 1; // 7 位地址 + 写位
                    s.sda_low = ((s.shift >> 7) & 1u) == 0;
                    s.bit_idx = 1;
                    break;
                case Phase::ADDR:
                    if (s.bit_idx == 0)
                        s.sda_low = ((s.shift >> 7) & 1u) == 0; // 高位先出
                    else
                        s.sda_low = ((s.shift >> (7 - s.bit_idx)) & 1u) == 0;
                    // 最后一位刚写进 sda_low，这里不能覆盖它，释放动作交给 SCL 高电平相位
                    if (++s.bit_idx >= 8)
                        s.phase = Phase::ADDR_ACK;
                    break;
                case Phase::ADDR_ACK:
                    if (s.ack_value) { // 从机没拉低 = NACK（取 SCL 高期间的采样值）
                        s.nack = true;
                        s.sda_low = true; // SCL 低时先把 SDA 拉低，下一半周期释放才构成 STOP
                        s.phase = Phase::STOP;
                    } else {
                        s.phase = Phase::DATA;
                        s.bit_idx = 1; // MSB 在进入 DATA 时已经送出，避免重复发一位
                        s.shift = s.tx_data & 0xFFu;
                        s.sda_low = ((s.shift >> 7) & 1u) == 0;
                    }
                    break;
                case Phase::DATA:
                    if (s.bit_idx == 0)
                        s.sda_low = ((s.shift >> 7) & 1u) == 0;
                    else
                        s.sda_low = ((s.shift >> (7 - s.bit_idx)) & 1u) == 0;
                    if (++s.bit_idx >= 8)
                        s.phase = Phase::DATA_ACK; // 同上，最后一位由 SCL 高电平相位释放
                    break;
                case Phase::DATA_ACK:
                    if (s.ack_value) // 取 SCL 高期间的采样值
                        s.nack = true;
                    // 此刻 SCL 是低电平，先把 SDA 拉低，下一半周期抬高 SCL 后才释放 SDA
                    s.sda_low = true;
                    s.phase = Phase::STOP;
                    break;
                case Phase::STOP: break; // SDA 在 SCL 高电平相位释放才构成停止条件
                default:
                    break;
                }
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
            case REG_RXDATA: dout = s.rx_data; break;
            case REG_STATUS: dout = ((s.phase != Phase::IDLE && s.phase != Phase::DONE) ? 1u : 0u) |
                                     (s.done ? 2u : 0u) | (s.nack ? 4u : 0u); break;
            case REG_ADDR: dout = s.addr; break;
            case REG_CLKDIV: dout = s.clkdiv; break;
            default: dout = 0; break;
            }
        } else {
            switch (off) {
            case REG_TXDATA:
                if (s.phase == Phase::IDLE) s.tx_data = wdat & 0xFFu; // 事务进行中不许换数据
                break;
            case REG_ADDR: s.addr = wdat & 0x7Fu; break;
            case REG_CLKDIV:
                if ((s.phase == Phase::IDLE || s.phase == Phase::DONE) && (wdat & 0xFFFFu))
                    s.clkdiv = wdat & 0xFFFFu;
                break;
            case REG_CONTROL:
                s.control = wdat;
                // DONE 表示上一次事务已经结束，必须允许再次启动，否则这个控制器只能用一次
                if ((wdat & 1u) && (s.phase == Phase::IDLE || s.phase == Phase::DONE)) {
                    s.phase = Phase::START;
                    s.nack = false;
                    s.done = false;
                    s.div_cnt = 0;
                    s.scl = true;
                    s.sda_low = true; // 起始条件：SCL 高时 SDA 拉低
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

    std::memset(outputs[O_DAT], 0, SLOT);
    std::memcpy(outputs[O_DAT], &dout, 4);
    outputs[O_ACK][0] = ack ? 1 : 0;
    outputs[O_SCL][0] = s.scl ? 1 : 0;
    outputs[O_SDA_OUT][0] = s.sda_low ? 0 : 1;
    outputs[O_SDA_OE][0] = s.sda_low ? 1 : 0;
}

} // namespace dsc
