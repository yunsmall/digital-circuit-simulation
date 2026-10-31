// DMA — 存储器到存储器的块搬运控制器
// 配置端口与搬运状态机全部展开在 JIT 里，每拍没有跨模块调用。
#include "dcs/components/platform/DMA.h"

#include <format>

namespace dsc {

namespace {
enum InIdx {
    I_CFG_ADR = 0, I_CFG_DAT, I_CFG_WE, I_CFG_SEL, I_CFG_STB, I_CFG_CYC,
    I_M_DAT_I, I_M_ACK_I
};
enum OutIdx {
    O_CFG_DAT = 0, O_CFG_ACK,
    O_M_ADR, O_M_DAT, O_M_WE, O_M_SEL, O_M_STB, O_M_CYC
};

constexpr uint32_t REG_SRC = 0x00, REG_DST = 0x04, REG_LEN = 0x08, REG_CTRL = 0x0C, REG_STATUS = 0x10;

// 搬运状态机取值
enum St { ST_IDLE = 0, ST_READ_REQ = 1, ST_READ_WAIT = 2, ST_WRITE_REQ = 3, ST_WRITE_WAIT = 4, ST_DONE = 5 };
} // namespace

DMA::DMA(const std::string &name) : SequentialComponent(name, "dma") {
    addInput("cfg_adr_i", 32);
    addInput("cfg_dat_i", 32);
    addInput("cfg_we_i", 1);
    addInput("cfg_sel_i", 4);
    addInput("cfg_stb_i", 1);
    addInput("cfg_cyc_i", 1);
    addInput("m_dat_i", 32);
    addInput("m_ack_i", 1);
    addOutput("cfg_dat_o", 32, false, true);
    addOutput("cfg_ack_o", 1, false, true);
    addOutput("m_adr_o", 32, false, true);
    addOutput("m_dat_o", 32, false, true);
    addOutput("m_we_o", 1, false, true);
    addOutput("m_sel_o", 4, false, true);
    addOutput("m_stb_o", 1, false, true);
    addOutput("m_cyc_o", 1, false, true);
}

DMA::~DMA() = default;

bool DMA::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::string DMA::genStateDecl() const {
    return std::format(
            "#define _DMA_{0}_SRC 0x00u\n"
            "#define _DMA_{0}_DST 0x04u\n"
            "#define _DMA_{0}_LEN 0x08u\n"
            "#define _DMA_{0}_CTRL 0x0Cu\n"
            "#define _DMA_{0}_STATUS 0x10u\n"
            "static uint32_t _dma_src_{0}, _dma_dst_{0}, _dma_len_{0}, _dma_remain_{0};\n"
            "static uint32_t _dma_rd_val_{0}, _dma_latched_{0};\n"
            "static uint32_t _dma_m_adr_{0}, _dma_m_dat_{0};\n"
            "static uint8_t  _dma_busy_{0}, _dma_done_{0};\n"
            "static uint8_t  _dma_m_we_{0}, _dma_m_stb_{0}, _dma_m_cyc_{0};\n"
            "static uint8_t  _dma_st_{0}, _dma_prev_stb_{0};\n",
            _id);
}

std::string DMA::genInitCode() const {
    return std::format(
            "    _dma_src_{0} = 0; _dma_dst_{0} = 0; _dma_len_{0} = 0; _dma_remain_{0} = 0;\n"
            "    _dma_rd_val_{0} = 0; _dma_latched_{0} = 0;\n"
            "    _dma_m_adr_{0} = 0; _dma_m_dat_{0} = 0;\n"
            "    _dma_busy_{0} = 0; _dma_done_{0} = 0;\n"
            "    _dma_m_we_{0} = 0; _dma_m_stb_{0} = 0; _dma_m_cyc_{0} = 0;\n"
            "    _dma_st_{0} = 0; _dma_prev_stb_{0} = 0;\n",
            _id);
}

std::string DMA::genFuncDef_seq() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    std::string c;
    c += std::format("// DMA {}\n", name());
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    c += "    uint32_t _cfg_adr = 0; dcs_memcpy(&_cfg_adr, _w[" + nid(I_CFG_ADR) + "], 4);\n";
    c += "    uint32_t _cfg_dat = 0; dcs_memcpy(&_cfg_dat, _w[" + nid(I_CFG_DAT) + "], 4);\n";
    c += "    uint8_t _cfg_we = _w[" + nid(I_CFG_WE) + "][0] & 1;\n";
    c += "    uint8_t _cfg_stb = _w[" + nid(I_CFG_STB) + "][0] & 1;\n";
    c += "    uint8_t _cfg_cyc = _w[" + nid(I_CFG_CYC) + "][0] & 1;\n";
    c += "    uint32_t _m_dati = 0; dcs_memcpy(&_m_dati, _w[" + nid(I_M_DAT_I) + "], 4);\n";
    c += "    uint8_t _m_acki = _w[" + nid(I_M_ACK_I) + "][0] & 1;\n";
    c += std::format(
            "    uint32_t _dout = 0; uint8_t _ack = 0;\n"
            "    const bool _cfg_active = _cfg_stb && _cfg_cyc;\n"
            "    const bool _cfg_rise = _cfg_active && !_dma_prev_stb_{0};\n"
            "    _dma_prev_stb_{0} = _cfg_active;\n"
            "    if (_cfg_rise) {{\n"
            "        switch (_cfg_adr & 0x1Fu) {{\n"
            "        case _DMA_{0}_SRC:\n"
            "            if (!_cfg_we) _dout = _dma_src_{0};\n"
            "            else if (!_dma_busy_{0}) _dma_src_{0} = _cfg_dat;\n"
            "            break;\n"
            "        case _DMA_{0}_DST:\n"
            "            if (!_cfg_we) _dout = _dma_dst_{0};\n"
            "            else if (!_dma_busy_{0}) _dma_dst_{0} = _cfg_dat;\n"
            "            break;\n"
            "        case _DMA_{0}_LEN:\n"
            "            if (!_cfg_we) _dout = _dma_len_{0};\n"
            "            else if (!_dma_busy_{0}) _dma_len_{0} = _cfg_dat & ~3u;\n" // 按字搬运，向下取整到 4
            "            break;\n"
            "        case _DMA_{0}_CTRL:\n"
            "            if (_cfg_we && (_cfg_dat & 1u) && !_dma_busy_{0} && _dma_len_{0}) {{\n"
            "                _dma_remain_{0} = _dma_len_{0}; _dma_busy_{0} = 1; _dma_done_{0} = 0; _dma_st_{0} = 1;\n"
            "            }}\n"
            "            break;\n"
            "        case _DMA_{0}_STATUS:\n"
            "            _dout = (_dma_busy_{0} ? 1u : 0u) | (_dma_done_{0} ? 2u : 0u);\n"
            "            break;\n"
            "        default: break;\n"
            "        }}\n"
            "        _ack = 1;\n"
            "        _dma_latched_{0} = _dout;\n"
            "    }} else if (_cfg_active) {{\n"
            "        _dout = _dma_latched_{0};\n"
            "        _ack = 1;\n"
            "    }}\n"
            "    if (!_cfg_active) _dma_latched_{0} = 0;\n",
            _id);
    // 读一个字再写一个字，地址各自递增 4
    c += std::format(
            "    if (_dma_st_{0} == 0 || _dma_st_{0} == 5) {{\n"
            "        _dma_m_stb_{0} = 0; _dma_m_cyc_{0} = 0;\n"
            "    }} else if (_dma_st_{0} == 1) {{\n"
            "        _dma_m_adr_{0} = _dma_src_{0};\n"
            "        _dma_m_we_{0} = 0;\n"
            "        _dma_m_stb_{0} = 1; _dma_m_cyc_{0} = 1;\n"
            "        _dma_st_{0} = 2;\n"
            "    }} else if (_dma_st_{0} == 2) {{\n"
            "        if (_m_acki) {{\n"
            "            _dma_rd_val_{0} = _m_dati;\n"
            "            _dma_m_stb_{0} = 0; _dma_m_cyc_{0} = 0;\n"
            "            _dma_st_{0} = 3;\n"
            "        }}\n"
            "    }} else if (_dma_st_{0} == 3) {{\n"
            "        _dma_m_adr_{0} = _dma_dst_{0};\n"
            "        _dma_m_dat_{0} = _dma_rd_val_{0};\n"
            "        _dma_m_we_{0} = 1;\n"
            "        _dma_m_stb_{0} = 1; _dma_m_cyc_{0} = 1;\n"
            "        _dma_st_{0} = 4;\n"
            "    }} else if (_dma_st_{0} == 4) {{\n"
            "        if (_m_acki) {{\n"
            "            _dma_m_stb_{0} = 0; _dma_m_cyc_{0} = 0;\n"
            "            _dma_src_{0} += 4; _dma_dst_{0} += 4;\n"
            "            if (_dma_remain_{0} >= 4) _dma_remain_{0} -= 4;\n"
            "            if (_dma_remain_{0} == 0) {{ _dma_busy_{0} = 0; _dma_done_{0} = 1; _dma_st_{0} = 5; }}\n"
            "            else _dma_st_{0} = 1;\n"
            "        }}\n"
            "    }}\n",
            _id);
    c += "    const uint8_t _msel = 0x0F;\n"; // 搬运按整字进行，四个字节全有效
    c += "    " + genOutputWrite(O_CFG_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_CFG_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_M_ADR, "_dma_m_adr_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_M_DAT, "_dma_m_dat_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_M_WE, "_dma_m_we_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_M_SEL, "_msel", 4) + "\n";
    c += "    " + genOutputWrite(O_M_STB, "_dma_m_stb_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_M_CYC, "_dma_m_cyc_" + std::to_string(_id), 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> DMA::clone(const std::string &new_name) const {
    return std::make_unique<DMA>(new_name);
}

} // namespace dsc
