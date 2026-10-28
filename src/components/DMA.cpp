#include "dcs/components/DMA.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
enum InIdx {
    I_CFG_ADR = 0, I_CFG_DAT, I_CFG_WE, I_CFG_SEL, I_CFG_STB, I_CFG_CYC,
    I_M_DAT_I, I_M_ACK_I
};
enum OutIdx {
    O_CFG_DAT = 0, O_CFG_ACK,
    O_M_ADR, O_M_DAT, O_M_WE, O_M_SEL, O_M_STB, O_M_CYC, O_COUNT
};

constexpr uint32_t REG_SRC = 0x00, REG_DST = 0x04, REG_LEN = 0x08, REG_CTRL = 0x0C, REG_STATUS = 0x10;
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

std::vector<Component::JitSymbol> DMA::extraJitSymbols() const {
    return {{"_dcs_dma_tick", reinterpret_cast<void *>(&DMA::dmaTick)},
            {"_dcs_dma_reset", reinterpret_cast<void *>(&DMA::dmaReset)}};
}

void DMA::dmaReset(void *state_ptr) {
    auto &s = *static_cast<DmaState *>(state_ptr);
    std::memset(&s, 0, sizeof(s));
    s.st = St::IDLE;
}

std::string DMA::genStateDecl() const {
    return std::format("static void *_dma_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string DMA::genInitCode() const { return std::format("    _dcs_dma_reset(_dma_state_{});\n", _id); }

std::string DMA::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_dma_tick(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_dma_reset(void *);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += genInputPtrArray("_in");
    // 输出先落中间缓冲，再由 genOutputWrite 同时写 _c_out 与 _w；
    // 传给回调的必须是指针数组，二维数组强转会让回调把首字节当指针
    code += std::format("    uint8_t _obuf[{}][{}];\n", (int) O_COUNT, SLOT);
    code += "    dcs_memset(_obuf, 0, sizeof(_obuf));\n";
    code += std::format("    uint8_t *_op[{}] = {{", (int) O_COUNT);
    for (int i = 0; i < O_COUNT; i++) code += std::format("{}_obuf[{}]", i ? "," : "", i);
    code += "};\n";
    code += std::format("    _dcs_dma_tick(_dma_state_{0}, _in, _op);\n", _id);
    code += "    { uint32_t _v=0; dcs_memcpy(&_v,_obuf[0],4); " + genOutputWrite(O_CFG_DAT, "_v", 32) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_obuf[1],1); " + genOutputWrite(O_CFG_ACK, "_v", 1) + " }\n";
    code += "    { uint32_t _v=0; dcs_memcpy(&_v,_obuf[2],4); " + genOutputWrite(O_M_ADR, "_v", 32) + " }\n";
    code += "    { uint32_t _v=0; dcs_memcpy(&_v,_obuf[3],4); " + genOutputWrite(O_M_DAT, "_v", 32) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_obuf[4],1); " + genOutputWrite(O_M_WE, "_v", 1) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_obuf[5],1); " + genOutputWrite(O_M_SEL, "_v", 4) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_obuf[6],1); " + genOutputWrite(O_M_STB, "_v", 1) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_obuf[7],1); " + genOutputWrite(O_M_CYC, "_v", 1) + " }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> DMA::clone(const std::string &new_name) const {
    auto p = std::make_unique<DMA>(new_name);
    p->_state = _state;
    return p;
}

void DMA::dmaTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &s = *static_cast<DmaState *>(state_ptr);

    uint32_t cfg_adr = 0, cfg_dat = 0, m_dat_i = 0;
    std::memcpy(&cfg_adr, inputs[I_CFG_ADR], 4);
    std::memcpy(&cfg_dat, inputs[I_CFG_DAT], 4);
    std::memcpy(&m_dat_i, inputs[I_M_DAT_I], 4);
    bool cfg_we = (inputs[I_CFG_WE][0] & 1) != 0;
    bool cfg_stb = (inputs[I_CFG_STB][0] & 1) != 0;
    bool cfg_cyc = (inputs[I_CFG_CYC][0] & 1) != 0;
    bool m_ack_i = (inputs[I_M_ACK_I][0] & 1) != 0;

    // ---- 配置端口 ----
    bool cfg_active = cfg_stb && cfg_cyc;
    bool cfg_rise = cfg_active && !s.prev_cfg_stb;
    s.prev_cfg_stb = cfg_active;

    uint32_t dout = 0;
    bool ack = false;
    if (cfg_rise) {
        switch (cfg_adr & 0x1Fu) {
        case REG_SRC:
            if (!cfg_we) dout = s.src;
            else if (!s.busy) s.src = cfg_dat;
            break;
        case REG_DST:
            if (!cfg_we) dout = s.dst;
            else if (!s.busy) s.dst = cfg_dat;
            break;
        case REG_LEN:
            if (!cfg_we) dout = s.len;
            else if (!s.busy) s.len = cfg_dat & ~3u; // 按字搬运，长度向下取整到 4
            break;
        case REG_CTRL:
            if (cfg_we && (cfg_dat & 1u) && !s.busy && s.len) {
                s.remain = s.len;
                s.busy = true;
                s.done = false;
                s.st = St::READ_REQ;
            }
            break;
        case REG_STATUS: dout = (s.busy ? 1u : 0u) | (s.done ? 2u : 0u); break;
        default: break;
        }
        ack = true;
        s.latched = dout;
    } else if (cfg_active) {
        dout = s.latched;
        ack = true;
    }
    if (!cfg_active)
        s.latched = 0;

    // ---- 搬运状态机 ----
    switch (s.st) {
    case St::IDLE:
    case St::DONE:
        s.m_stb = s.m_cyc = false;
        break;
    case St::READ_REQ:
        s.m_adr = s.src;
        s.m_we = false;
        s.m_stb = s.m_cyc = true;
        s.st = St::READ_WAIT;
        break;
    case St::READ_WAIT:
        if (m_ack_i) {
            s.rd_val = m_dat_i;
            s.m_stb = s.m_cyc = false;
            s.st = St::WRITE_REQ;
        }
        break;
    case St::WRITE_REQ:
        s.m_adr = s.dst;
        s.m_dat = s.rd_val;
        s.m_we = true;
        s.m_stb = s.m_cyc = true;
        s.st = St::WRITE_WAIT;
        break;
    case St::WRITE_WAIT:
        if (m_ack_i) {
            s.m_stb = s.m_cyc = false;
            s.src += 4;
            s.dst += 4;
            if (s.remain >= 4)
                s.remain -= 4;
            if (s.remain == 0) {
                s.busy = false;
                s.done = true;
                s.st = St::DONE;
            } else {
                s.st = St::READ_REQ;
            }
        }
        break;
    }

    std::memset(outputs[O_CFG_DAT], 0, SLOT);
    std::memcpy(outputs[O_CFG_DAT], &dout, 4);
    outputs[O_CFG_ACK][0] = ack ? 1 : 0;
    std::memcpy(outputs[O_M_ADR], &s.m_adr, 4);
    std::memcpy(outputs[O_M_DAT], &s.m_dat, 4);
    outputs[O_M_WE][0] = s.m_we ? 1 : 0;
    outputs[O_M_SEL][0] = 0x0F;
    outputs[O_M_STB][0] = s.m_stb ? 1 : 0;
    outputs[O_M_CYC][0] = s.m_cyc ? 1 : 0;
}

} // namespace dsc
