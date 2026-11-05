// SPI — 模式 0 的 SPI 控制器（Wishbone 从设备）
// 位时序和寄存器组全部展开在 JIT 里，每拍没有跨模块调用。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/SPI.h"

#include <format>

namespace dsc {

namespace {
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_MISO };
enum OutIdx { O_DAT = 0, O_ACK, O_SCLK, O_MOSI, O_CS };

constexpr uint32_t REG_TXDATA = 0x00;
constexpr uint32_t REG_RXDATA = 0x04;
constexpr uint32_t REG_STATUS = 0x08;
constexpr uint32_t REG_CONTROL = 0x0C;
constexpr uint32_t REG_CLKDIV = 0x10;
} // namespace

SPI::SPI(const std::string &name, int clkdiv) : SequentialComponent(name, "spi"), _clkdiv(clkdiv) {
    // 0 及以下钳到 1：这是有意的容错（测试 ZeroClkdiv 覆盖），setParam 记录生效值
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


std::string SPI::genStateDecl() const {
    return std::format(
            "#define _SPI_{0}_TXDATA 0x00u\n"
            "#define _SPI_{0}_RXDATA 0x04u\n"
            "#define _SPI_{0}_STATUS 0x08u\n"
            "#define _SPI_{0}_CONTROL 0x0Cu\n"
            "#define _SPI_{0}_CLKDIV 0x10u\n"
            "static uint32_t _spi_div_{0}, _spi_bits_{0}, _spi_tx_{0}, _spi_rx_{0};\n"
            "static uint32_t _spi_rxdata_{0}, _spi_ctrl_{0}, _spi_latched_{0}, _spi_clkdiv_{0};\n"
            "static uint8_t  _spi_busy_{0}, _spi_sclk_{0}, _spi_done_{0}, _spi_prev_stb_{0};\n",
            _id);
}

std::string SPI::genInitCode() const {
    return std::format(
            "    _spi_div_{0} = 0; _spi_bits_{0} = 0; _spi_tx_{0} = 0; _spi_rx_{0} = 0;\n"
            "    _spi_rxdata_{0} = 0; _spi_ctrl_{0} = 0; _spi_latched_{0} = 0; _spi_clkdiv_{0} = {1};\n"
            "    _spi_busy_{0} = 0; _spi_sclk_{0} = 0; _spi_done_{0} = 0; _spi_prev_stb_{0} = 0;\n",
            _id, _clkdiv);
}

std::string SPI::genFuncDef_seq() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    std::string c;
    c += std::format("// SPI {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(I_ADR) + "], 4);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _stb = _w[" + nid(I_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(I_CYC) + "][0] & 1;\n";
    c += "    uint8_t _miso = _w[" + nid(I_MISO) + "][0] & 1;\n";
    c += std::format(
            "    const bool _active = _stb && _cyc;\n"
            "    const bool _rise = _active && !_spi_prev_stb_{0};\n"
            "    _spi_prev_stb_{0} = _active;\n"
            // 位时序：每 clkdiv 个 tick 翻转一次 sclk，下降沿移出下一位，上升沿采样
            "    if (_spi_busy_{0}) {{\n"
            "        if (++_spi_div_{0} >= _spi_clkdiv_{0}) {{\n"
            "            _spi_div_{0} = 0;\n"
            "            _spi_sclk_{0} = !_spi_sclk_{0};\n"
            "            if (!_spi_sclk_{0}) {{\n"
            "                if (_spi_bits_{0} > 0) {{\n"
            "                    _spi_bits_{0}--;\n"
            "                    if (_spi_bits_{0} == 0) {{\n"
            "                        _spi_busy_{0} = 0; _spi_done_{0} = 1;\n"
            "                        _spi_rxdata_{0} = _spi_rx_{0} & 0xFFu;\n"
            "                    }}\n"
            "                }}\n"
            "            }} else {{\n"
            "                _spi_rx_{0} = (_spi_rx_{0} << 1) | (_miso ? 1u : 0u);\n"
            "            }}\n"
            "        }}\n"
            "    }}\n"
            "    uint32_t _dout = 0; uint8_t _ack = 0;\n"
            "    if (_rise) {{\n"
            "        switch (_adr & 0x1Fu) {{\n",
            _id);
    // 读寄存器
    c += std::format(
            "        case _SPI_{0}_RXDATA: if (!_we) {{ _dout = _spi_rxdata_{0}; _spi_done_{0} = 0; }} break;\n"
            "        case _SPI_{0}_STATUS:\n"
            "            if (!_we) _dout = (_spi_busy_{0} ? 1u : 0u) | (_spi_done_{0} ? 2u : 0u);\n"
            "            break;\n"
            "        case _SPI_{0}_CLKDIV: if (!_we) _dout = _spi_clkdiv_{0}; break;\n"
            "        case _SPI_{0}_CONTROL: if (!_we) _dout = _spi_ctrl_{0}; break;\n"
            "        default: break;\n"
            "        }}\n",
            _id);
    // 写寄存器
    c += std::format(
            "        if (_we) {{\n"
            "            switch (_adr & 0x1Fu) {{\n"
            "            case _SPI_{0}_TXDATA:\n"
            "                if (!_spi_busy_{0}) _spi_tx_{0} = _wdat & 0xFFu;\n" // 传输中改写会破坏当前字节
            "                break;\n"
            "            case _SPI_{0}_CLKDIV:\n"
            "                if (!_spi_busy_{0} && (_wdat & 0xFFFFu)) _spi_clkdiv_{0} = _wdat & 0xFFFFu;\n"
            "                break;\n"
            "            case _SPI_{0}_CONTROL:\n"
            "                _spi_ctrl_{0} = _wdat;\n"
            "                if ((_wdat & 1u) && !_spi_busy_{0}) {{\n" // 启动一次传输
            "                    _spi_busy_{0} = 1; _spi_done_{0} = 0; _spi_bits_{0} = 8;\n"
            "                    _spi_rx_{0} = 0; _spi_div_{0} = 0; _spi_sclk_{0} = 0;\n"
            "                }}\n"
            "                break;\n"
            "            default: break;\n"
            "            }}\n"
            "        }}\n"
            "        _ack = 1;\n"
            "        _spi_latched_{0} = _dout;\n"
            "    }} else if (_active) {{\n"
            "        _dout = _spi_latched_{0};\n"
            "        _ack = 1;\n"
            "    }}\n"
            "    if (!_active) _spi_latched_{0} = 0;\n"
            // 模式 0：空闲低，传输期间 cs 拉低，发送最高位
            "    const uint32_t _mosi = _spi_busy_{0} ? ((_spi_tx_{0} >> (_spi_bits_{0} ? _spi_bits_{0} - 1 : 7)) & 1u) : 0u;\n"
            "    const uint32_t _cs = _spi_busy_{0} ? 0u : 1u;\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_SCLK, "_spi_sclk_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_MOSI, "_mosi", 1) + "\n";
    c += "    " + genOutputWrite(O_CS, "_cs", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> SPI::clone(const std::string &new_name) const {
    return std::make_unique<SPI>(new_name, _clkdiv);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"spi", {{"clkdiv", "4"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "miso"}, {"wb_dat_o", "wb_ack_o", "sclk", "mosi", "cs"}, dsc::ComponentCategory::Misc, "SPI 主机（模式 0），按 clkdiv 分频收发 8 位"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::SPI>(n, dsc::paramInt(p, "clkdiv", 4));
        });
} // namespace
