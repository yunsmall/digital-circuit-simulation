#include "dcs/components/AxiLiteBridge.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
enum InIdx {
    I_AWADDR = 0, I_AWVALID, I_WDATA, I_WSTRB, I_WVALID, I_BREADY,
    I_ARADDR, I_ARVALID, I_RREADY, I_WB_DAT, I_WB_ACK, I_COUNT
};
enum OutIdx {
    O_AWREADY = 0, O_WREADY, O_BRESP, O_BVALID, O_ARREADY, O_RDATA, O_RRESP, O_RVALID,
    O_WB_ADR, O_WB_DAT, O_WB_WE, O_WB_SEL, O_WB_STB, O_WB_CYC, O_COUNT
};
constexpr uint8_t RESP_OKAY = 0;
} // namespace

AxiLiteBridge::AxiLiteBridge(const std::string &name) : SequentialComponent(name, "axi_lite_bridge") {
    addInput("s_axi_awaddr", 32);
    addInput("s_axi_awvalid", 1);
    addInput("s_axi_wdata", 32);
    addInput("s_axi_wstrb", 4);
    addInput("s_axi_wvalid", 1);
    addInput("s_axi_bready", 1);
    addInput("s_axi_araddr", 32);
    addInput("s_axi_arvalid", 1);
    addInput("s_axi_rready", 1);
    addInput("wb_dat_i", 32);
    addInput("wb_ack_i", 1);
    addOutput("s_axi_awready", 1, false, true);
    addOutput("s_axi_wready", 1, false, true);
    addOutput("s_axi_bresp", 2, false, true);
    addOutput("s_axi_bvalid", 1, false, true);
    addOutput("s_axi_arready", 1, false, true);
    addOutput("s_axi_rdata", 32, false, true);
    addOutput("s_axi_rresp", 2, false, true);
    addOutput("s_axi_rvalid", 1, false, true);
    addOutput("wb_adr_o", 32, false, true);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_we_o", 1, false, true);
    addOutput("wb_sel_o", 4, false, true);
    addOutput("wb_stb_o", 1, false, true);
    addOutput("wb_cyc_o", 1, false, true);
}

AxiLiteBridge::~AxiLiteBridge() = default;

bool AxiLiteBridge::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> AxiLiteBridge::extraJitSymbols() const {
    return {{"_dcs_axib_tick", reinterpret_cast<void *>(&AxiLiteBridge::bridgeTick)},
            {"_dcs_axib_reset", reinterpret_cast<void *>(&AxiLiteBridge::bridgeReset)}};
}

void AxiLiteBridge::bridgeReset(void *state_ptr) {
    auto &s = *static_cast<BridgeState *>(state_ptr);
    std::memset(&s, 0, sizeof(s));
    s.st = St::IDLE;
}

std::string AxiLiteBridge::genStateDecl() const {
    return std::format("static void *_axib_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string AxiLiteBridge::genInitCode() const { return std::format("    _dcs_axib_reset(_axib_state_{});\n", _id); }

std::string AxiLiteBridge::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_axib_tick(void *, const uint8_t *, uint8_t *);\n";
    code += "extern void _dcs_axib_reset(void *);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += std::format("    uint8_t _in[{}][{}];\n", (int) I_COUNT, SLOT);
    for (int i = 0; i < I_COUNT; i++) {
        int w = (i == I_AWADDR || i == I_WDATA || i == I_ARADDR || i == I_WB_DAT) ? 32
                : (i == I_WSTRB ? 4 : 1);
        code += std::format("    {{ uint64_t _v=0; dcs_memcpy(&_v,_w[{}],{}); uint8_t _b[8]; dcs_memcpy(_b,&_v,8); "
                            "dcs_memcpy(_in[{}],_b,{}); }}\n",
                            inputs()[i]->netId(), byte_count(w), i, byte_count(w));
    }
    code += std::format("    uint8_t _out[{}][{}];\n", (int) O_COUNT, SLOT);
    code += std::format("    _dcs_axib_tick(_axib_state_{0}, (const uint8_t*)_in, (uint8_t*)_out);\n", _id);
    const int widths[O_COUNT] = {1, 1, 2, 1, 1, 32, 2, 1, 32, 32, 1, 4, 1, 1};
    for (int i = 0; i < O_COUNT; i++) {
        const std::string var = std::format("_v{}", i);
        code += std::format("    {{ uint32_t _v=0; dcs_memcpy(&_v,_out[{}],4); {} }}\n", i,
                            genOutputWrite(i, "_v", widths[i]));
    }
    code += "}\n";
    return code;
}

std::unique_ptr<Component> AxiLiteBridge::clone(const std::string &new_name) const {
    auto p = std::make_unique<AxiLiteBridge>(new_name);
    p->_state = _state;
    return p;
}

void AxiLiteBridge::bridgeTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs) {
    auto &s = *static_cast<BridgeState *>(state_ptr);

    uint32_t awaddr = 0, wdata = 0, araddr = 0, wb_dat_i = 0;
    std::memcpy(&awaddr, inputs + I_AWADDR * SLOT, 4);
    std::memcpy(&wdata, inputs + I_WDATA * SLOT, 4);
    std::memcpy(&araddr, inputs + I_ARADDR * SLOT, 4);
    std::memcpy(&wb_dat_i, inputs + I_WB_DAT * SLOT, 4);
    bool awvalid = (inputs[I_AWVALID * SLOT] & 1) != 0;
    uint8_t wstrb = inputs[I_WSTRB * SLOT];
    bool wvalid = (inputs[I_WVALID * SLOT] & 1) != 0;
    bool bready = (inputs[I_BREADY * SLOT] & 1) != 0;
    bool arvalid = (inputs[I_ARVALID * SLOT] & 1) != 0;
    bool rready = (inputs[I_RREADY * SLOT] & 1) != 0;
    bool wb_ack = (inputs[I_WB_ACK * SLOT] & 1) != 0;

    bool awready = false, wready = false, arready = false;

    // ---- 接收 AW / W ----
    if (!s.aw_got && awvalid && s.st == St::IDLE) {
        s.aw_addr = awaddr;
        s.aw_got = true;
        awready = true;
    }
    if (!s.w_got && wvalid && s.st == St::IDLE) {
        s.w_data = wdata;
        s.w_strb = wstrb;
        s.w_got = true;
        wready = true;
    }

    switch (s.st) {
    case St::IDLE:
        if (s.aw_got && s.w_got) {
            s.wb_adr = s.aw_addr;
            s.wb_dat = s.w_data;
            s.wb_we = true;
            s.wb_stb = s.wb_cyc = true;
            s.st = St::WRITE_WAIT;
            s.aw_got = s.w_got = false;
        } else if (arvalid) {
            s.wb_adr = araddr;
            s.wb_we = false;
            s.wb_stb = s.wb_cyc = true;
            arready = true;
            s.st = St::READ_WAIT;
        }
        break;
    case St::WRITE_WAIT:
        if (wb_ack) {
            s.wb_stb = s.wb_cyc = false;
            s.bresp = RESP_OKAY;
            s.bvalid = true;
            s.st = St::WRITE_RESP;
        }
        break;
    case St::WRITE_RESP:
        if (bready) {
            s.bvalid = false;
            s.st = St::IDLE;
        }
        break;
    case St::READ_WAIT:
        if (wb_ack) {
            s.r_data = wb_dat_i;
            s.wb_stb = s.wb_cyc = false;
            s.rresp = RESP_OKAY;
            s.rvalid = true;
            s.st = St::READ_RESP;
        }
        break;
    case St::READ_RESP:
        if (rready) {
            s.rvalid = false;
            s.st = St::IDLE;
        }
        break;
    default: break;
    }

    std::memset(outputs, 0, O_COUNT * SLOT);
    outputs[O_AWREADY * SLOT] = awready ? 1 : 0;
    outputs[O_WREADY * SLOT] = wready ? 1 : 0;
    outputs[O_BRESP * SLOT] = s.bresp;
    outputs[O_BVALID * SLOT] = s.bvalid ? 1 : 0;
    outputs[O_ARREADY * SLOT] = arready ? 1 : 0;
    std::memcpy(outputs + O_RDATA * SLOT, &s.r_data, 4);
    outputs[O_RRESP * SLOT] = s.rresp;
    outputs[O_RVALID * SLOT] = s.rvalid ? 1 : 0;
    std::memcpy(outputs + O_WB_ADR * SLOT, &s.wb_adr, 4);
    std::memcpy(outputs + O_WB_DAT * SLOT, &s.wb_dat, 4);
    outputs[O_WB_WE * SLOT] = s.wb_we ? 1 : 0;
    outputs[O_WB_SEL * SLOT] = s.wb_we ? s.w_strb : 0x0F; // 写事务必须按 AXI 的字节使能来
    outputs[O_WB_STB * SLOT] = s.wb_stb ? 1 : 0;
    outputs[O_WB_CYC * SLOT] = s.wb_cyc ? 1 : 0;
}

} // namespace dsc
