// I2C — 主机模式 I2C 控制器（Wishbone 从设备）
// 事务相位机、位时序和寄存器组全部展开在 JIT 里，每拍没有跨模块调用。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/I2C.h"

#include <format>

namespace dsc {

namespace {
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_SDA_IN };
enum OutIdx { O_DAT = 0, O_ACK, O_SCL, O_SDA_OUT, O_SDA_OE };

constexpr uint32_t REG_TXDATA = 0x00;
constexpr uint32_t REG_RXDATA = 0x04;
constexpr uint32_t REG_STATUS = 0x08;
constexpr uint32_t REG_CONTROL = 0x0C;
constexpr uint32_t REG_ADDR = 0x10;
constexpr uint32_t REG_CLKDIV = 0x14;

// 事务相位：起始 -> 地址 8 位 -> ACK -> 数据 8 位 -> ACK -> 停止
enum Ph { PH_IDLE = 0, PH_START = 1, PH_ADDR = 2, PH_ADDR_ACK = 3, PH_DATA = 4, PH_DATA_ACK = 5, PH_STOP = 6, PH_DONE = 7 };
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

bool I2C::prepare(Context &, std::string &) {
    _prepared = true;
    return true;
}

std::string I2C::genStateDecl() const {
    return std::format(
            "#define _I2C_{0}_TXDATA 0x00u\n"
            "#define _I2C_{0}_RXDATA 0x04u\n"
            "#define _I2C_{0}_STATUS 0x08u\n"
            "#define _I2C_{0}_CONTROL 0x0Cu\n"
            "#define _I2C_{0}_ADDR 0x10u\n"
            "#define _I2C_{0}_CLKDIV 0x14u\n"
            "static uint32_t _i2c_clkdiv_{0}, _i2c_div_{0}, _i2c_shift_{0}, _i2c_tx_{0};\n"
            "static uint32_t _i2c_rx_{0}, _i2c_addr_{0}, _i2c_ctrl_{0}, _i2c_latched_{0};\n"
            "static uint8_t  _i2c_phase_{0}, _i2c_scl_{0}, _i2c_bit_idx_{0};\n"
            "static uint8_t  _i2c_nack_{0}, _i2c_done_{0}, _i2c_sda_low_{0};\n"
            "static uint8_t  _i2c_ack_sampled_{0}, _i2c_ack_value_{0}, _i2c_prev_stb_{0};\n",
            _id);
}

std::string I2C::genInitCode() const {
    return std::format(
            "    _i2c_clkdiv_{0} = {1}; _i2c_div_{0} = 0; _i2c_shift_{0} = 0; _i2c_tx_{0} = 0;\n"
            "    _i2c_rx_{0} = 0; _i2c_addr_{0} = 0; _i2c_ctrl_{0} = 0; _i2c_latched_{0} = 0;\n"
            "    _i2c_phase_{0} = 0; _i2c_bit_idx_{0} = 0;\n"
            "    _i2c_nack_{0} = 0; _i2c_done_{0} = 0; _i2c_sda_low_{0} = 0;\n"
            "    _i2c_ack_sampled_{0} = 0; _i2c_ack_value_{0} = 0; _i2c_prev_stb_{0} = 0;\n"
            // I2C 总线空闲时 SCL 与 SDA 都被上拉为高
            "    _i2c_scl_{0} = 1;\n",
            _id, _clkdiv);
}

std::string I2C::genFuncDef_seq() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    std::string c;
    c += std::format("// I2C {}\n", name());
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(I_ADR) + "], 4);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _stb = _w[" + nid(I_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(I_CYC) + "][0] & 1;\n";
    c += "    uint8_t _sda_in = _w[" + nid(I_SDA_IN) + "][0] & 1;\n";
    c += std::format(
            "    const bool _active = _stb && _cyc;\n"
            "    const bool _rise = _active && !_i2c_prev_stb_{0};\n"
            "    _i2c_prev_stb_{0} = _active;\n"
            // ACK 位要在 SCL 高电平期间采样：在下降沿才读的话，纯组合的从机会在同一拍
            // 看到 SCL 变低就释放 SDA，主机读到的就是上拉高电平，把 ACK 误判成 NACK
            "    if (_i2c_scl_{0} && !_i2c_ack_sampled_{0} && (_i2c_phase_{0} == 3 || _i2c_phase_{0} == 5)\n"
            "        && _i2c_div_{0} >= _i2c_clkdiv_{0} / 2) {{\n"
            "        _i2c_ack_value_{0} = _sda_in;\n"
            "        _i2c_ack_sampled_{0} = 1;\n"
            "    }}\n"
            // 事务推进：每 clkdiv 个 tick 走一个 SCL 半周期
            "    if (_i2c_phase_{0} != 0 && _i2c_phase_{0} != 7) {{\n"
            "        if (++_i2c_div_{0} >= _i2c_clkdiv_{0}) {{\n"
            "            _i2c_div_{0} = 0;\n"
            "            _i2c_scl_{0} = !_i2c_scl_{0};\n"
            "            if (_i2c_scl_{0}) {{\n"
            // 停止条件要求 SCL 高电平期间 SDA 由低变高，只能在这里释放
            "                if (_i2c_phase_{0} == 6) {{\n"
            "                    _i2c_sda_low_{0} = 0;\n"
            "                    _i2c_phase_{0} = 7; _i2c_done_{0} = 1;\n"
            "                }} else if (_i2c_phase_{0} == 3 || _i2c_phase_{0} == 5) {{\n"
            "                    _i2c_sda_low_{0} = 0;\n" // 主机释放 SDA，从机此时才能拉低表示 ACK
            "                    _i2c_ack_sampled_{0} = 0;\n"
            "                }}\n"
            "            }} else {{\n"
            // SCL 下降沿推进一位
            "                if (_i2c_phase_{0} == 1) {{\n"
            // 起始条件已在写 CONTROL 时建立（SCL 高、SDA 低），这里处在 SCL 下降沿，
            // 直接送出地址最高位，不能再动 scl
            "                    _i2c_phase_{0} = 2;\n"
            "                    _i2c_shift_{0} = (_i2c_addr_{0} & 0x7Fu) << 1;\n" // 7 位地址 + 写位
            "                    _i2c_sda_low_{0} = ((_i2c_shift_{0} >> 7) & 1u) == 0;\n"
            "                    _i2c_bit_idx_{0} = 1;\n"
            "                }} else if (_i2c_phase_{0} == 2) {{\n"
            "                    if (_i2c_bit_idx_{0} == 0) _i2c_sda_low_{0} = ((_i2c_shift_{0} >> 7) & 1u) == 0;\n"
            "                    else _i2c_sda_low_{0} = ((_i2c_shift_{0} >> (7 - _i2c_bit_idx_{0})) & 1u) == 0;\n"
            // 最后一位刚写进 sda_low，这里不能覆盖它，释放动作交给 SCL 高电平相位
            "                    if (++_i2c_bit_idx_{0} >= 8) _i2c_phase_{0} = 3;\n"
            "                }} else if (_i2c_phase_{0} == 3) {{\n"
            "                    if (_i2c_ack_value_{0}) {{\n" // 从机没拉低 = NACK
            "                        _i2c_nack_{0} = 1;\n"
            "                        _i2c_sda_low_{0} = 1;\n" // SCL 低时先拉低，下一半周期释放才构成 STOP
            "                        _i2c_phase_{0} = 6;\n"
            "                    }} else {{\n"
            "                        _i2c_phase_{0} = 4;\n"
            "                        _i2c_bit_idx_{0} = 1;\n" // MSB 在进入 DATA 时已经送出，避免重复发一位
            "                        _i2c_shift_{0} = _i2c_tx_{0} & 0xFFu;\n"
            "                        _i2c_sda_low_{0} = ((_i2c_shift_{0} >> 7) & 1u) == 0;\n"
            "                    }}\n"
            "                }} else if (_i2c_phase_{0} == 4) {{\n"
            "                    if (_i2c_bit_idx_{0} == 0) _i2c_sda_low_{0} = ((_i2c_shift_{0} >> 7) & 1u) == 0;\n"
            "                    else _i2c_sda_low_{0} = ((_i2c_shift_{0} >> (7 - _i2c_bit_idx_{0})) & 1u) == 0;\n"
            "                    if (++_i2c_bit_idx_{0} >= 8) _i2c_phase_{0} = 5;\n"
            "                }} else if (_i2c_phase_{0} == 5) {{\n"
            "                    if (_i2c_ack_value_{0}) _i2c_nack_{0} = 1;\n"
            // 此刻 SCL 是低电平，先把 SDA 拉低，下一半周期抬高 SCL 后才释放 SDA
            "                    _i2c_sda_low_{0} = 1;\n"
            "                    _i2c_phase_{0} = 6;\n"
            "                }}\n"
            "            }}\n"
            "        }}\n"
            "    }}\n"
            "    uint32_t _dout = 0; uint8_t _ack = 0;\n"
            "    if (_rise) {{\n"
            "        switch (_adr & 0x1Fu) {{\n",
            _id);
    // 读寄存器
    c += std::format(
            "        case _I2C_{0}_RXDATA: if (!_we) _dout = _i2c_rx_{0}; break;\n"
            "        case _I2C_{0}_STATUS:\n"
            "            if (!_we) _dout = ((_i2c_phase_{0} != 0 && _i2c_phase_{0} != 7) ? 1u : 0u) |\n"
            "                               (_i2c_done_{0} ? 2u : 0u) | (_i2c_nack_{0} ? 4u : 0u);\n"
            "            break;\n"
            "        case _I2C_{0}_ADDR: if (!_we) _dout = _i2c_addr_{0}; break;\n"
            "        case _I2C_{0}_CLKDIV: if (!_we) _dout = _i2c_clkdiv_{0}; break;\n"
            "        default: break;\n"
            "        }}\n",
            _id);
    // 写寄存器
    c += std::format(
            "        if (_we) {{\n"
            "            switch (_adr & 0x1Fu) {{\n"
            "            case _I2C_{0}_TXDATA:\n"
            "                if (_i2c_phase_{0} == 0) _i2c_tx_{0} = _wdat & 0xFFu;\n" // 事务进行中不许换数据
            "                break;\n"
            "            case _I2C_{0}_ADDR: _i2c_addr_{0} = _wdat & 0x7Fu; break;\n"
            "            case _I2C_{0}_CLKDIV:\n"
            "                if ((_i2c_phase_{0} == 0 || _i2c_phase_{0} == 7) && (_wdat & 0xFFFFu))\n"
            "                    _i2c_clkdiv_{0} = _wdat & 0xFFFFu;\n"
            "                break;\n"
            "            case _I2C_{0}_CONTROL:\n"
            "                _i2c_ctrl_{0} = _wdat;\n"
            // DONE 表示上一次事务已经结束，必须允许再次启动，否则这个控制器只能用一次
            "                if ((_wdat & 1u) && (_i2c_phase_{0} == 0 || _i2c_phase_{0} == 7)) {{\n"
            "                    _i2c_phase_{0} = 1; _i2c_nack_{0} = 0; _i2c_done_{0} = 0;\n"
            "                    _i2c_div_{0} = 0; _i2c_scl_{0} = 1; _i2c_sda_low_{0} = 1;\n" // 起始条件：SCL 高时 SDA 拉低
            "                }}\n"
            "                break;\n"
            "            default: break;\n"
            "            }}\n"
            "        }}\n"
            "        _ack = 1;\n"
            "        _i2c_latched_{0} = _dout;\n"
            "    }} else if (_active) {{\n"
            "        _dout = _i2c_latched_{0};\n"
            "        _ack = 1;\n"
            "    }}\n"
            "    if (!_active) _i2c_latched_{0} = 0;\n"
            // 开漏：主机拉低时输出 0 并使能，释放时输出 1 并关断
            "    const uint8_t _sda_out = _i2c_sda_low_{0} ? 0 : 1;\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_SCL, "_i2c_scl_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_SDA_OUT, "_sda_out", 1) + "\n";
    c += "    " + genOutputWrite(O_SDA_OE, "_i2c_sda_low_" + std::to_string(_id), 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> I2C::clone(const std::string &new_name) const {
    return std::make_unique<I2C>(new_name, _clkdiv);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"i2c", {{"clkdiv", "8"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "sda_in"}, {"wb_dat_o", "wb_ack_o", "scl", "sda_out", "sda_oe"}, dsc::ComponentCategory::Misc, "I2C 主机，一次起始-地址-数据-停止的写事务"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::I2C>(n, dsc::paramInt(p, "clkdiv", 8));
        });
} // namespace
