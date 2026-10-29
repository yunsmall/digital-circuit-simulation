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
    code += "extern void _dcs_axib_tick(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_axib_reset(void *);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += genInputPtrArray("_in");
    // 输出先落中间缓冲，再由 genOutputWrite 同时写 _c_out 与 _w；
    // 传给回调的必须是指针数组，二维数组强转会让回调把首字节当指针
    code += genOutputPtrArray((int) O_COUNT);
    code += std::format("    _dcs_axib_tick(_axib_state_{0}, _in, _op);\n", _id);
    const int widths[O_COUNT] = {1, 1, 2, 1, 1, 32, 2, 1, 32, 32, 1, 4, 1, 1};
    for (int i = 0; i < O_COUNT; i++)
        code += std::format("    {{ uint32_t _v=0; dcs_memcpy(&_v,_obuf[{}],4); {} }}\n", i,
                            genOutputWrite(i, "_v", widths[i]));
    code += "}\n";
    return code;
}

std::unique_ptr<Component> AxiLiteBridge::clone(const std::string &new_name) const {
    auto p = std::make_unique<AxiLiteBridge>(new_name);
    p->_state = _state;
    return p;
}

void AxiLiteBridge::bridgeTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &s = *static_cast<BridgeState *>(state_ptr);

    uint32_t awaddr = 0, wdata = 0, araddr = 0, wb_dat_i = 0;
    std::memcpy(&awaddr, inputs[I_AWADDR], 4);
    std::memcpy(&wdata, inputs[I_WDATA], 4);
    std::memcpy(&araddr, inputs[I_ARADDR], 4);
    std::memcpy(&wb_dat_i, inputs[I_WB_DAT], 4);
    bool awvalid = (inputs[I_AWVALID][0] & 1) != 0;
    uint8_t wstrb = inputs[I_WSTRB][0];
    bool wvalid = (inputs[I_WVALID][0] & 1) != 0;
    bool bready = (inputs[I_BREADY][0] & 1) != 0;
    bool arvalid = (inputs[I_ARVALID][0] & 1) != 0;
    bool rready = (inputs[I_RREADY][0] & 1) != 0;
    bool wb_ack = (inputs[I_WB_ACK][0] & 1) != 0;

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

    std::memset(outputs[O_AWREADY], 0, SLOT);
    outputs[O_AWREADY][0] = awready ? 1 : 0;
    outputs[O_WREADY][0] = wready ? 1 : 0;
    outputs[O_BRESP][0] = s.bresp;
    outputs[O_BVALID][0] = s.bvalid ? 1 : 0;
    outputs[O_ARREADY][0] = arready ? 1 : 0;
    std::memcpy(outputs[O_RDATA], &s.r_data, 4);
    outputs[O_RRESP][0] = s.rresp;
    outputs[O_RVALID][0] = s.rvalid ? 1 : 0;
    std::memcpy(outputs[O_WB_ADR], &s.wb_adr, 4);
    std::memcpy(outputs[O_WB_DAT], &s.wb_dat, 4);
    outputs[O_WB_WE][0] = s.wb_we ? 1 : 0;
    outputs[O_WB_SEL][0] = s.wb_we ? s.w_strb : 0x0F; // 写事务必须按 AXI 的字节使能来
    outputs[O_WB_STB][0] = s.wb_stb ? 1 : 0;
    outputs[O_WB_CYC][0] = s.wb_cyc ? 1 : 0;
}

} // namespace dsc
