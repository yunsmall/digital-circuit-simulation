// CLINT — RISC-V 核心局部中断器
#include "dcs/components/CLINT.h"
#include <cstring>
#include <format>

namespace dsc {

// Wishbone 引脚索引
constexpr int SLOT = 16;
constexpr int I_ADR = 0, I_WDAT = 1, I_WE = 2, I_SEL = 3, I_STB = 4, I_CYC = 5, I_CLK = 6;
constexpr int I_COUNT = 7;
constexpr int O_DAT = 0, O_ACK = 1, O_MTI = 2, O_MSI = 3;

// MMIO 偏移
constexpr uint32_t OFF_MSIP     = 0x0000;
constexpr uint32_t OFF_MTIMECMP = 0x4000;
constexpr uint32_t OFF_MTIME    = 0xBFF8;

CLINT::CLINT(const std::string &name) : SequentialComponent(name, "clint") {
    // Wishbone 从设备输入（必须与 I_* 顺序一致）
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("clk", 1);

    // Wishbone 从设备输出 + 中断信号
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("mti", 1, false, true);  // Machine Timer Interrupt
    addOutput("msi", 1, false, true);  // Machine Software Interrupt

    std::memset(&_state, 0, sizeof(_state));
}

bool CLINT::prepare(std::string &) { _prepared = true; return true; }

std::vector<Component::JitSymbol> CLINT::extraJitSymbols() const {
    return {{"_dcs_clint_tick", reinterpret_cast<void *>(&CLINT::clintTick)}};
}

std::string CLINT::genStateDecl() const {
    return std::format("static void *_clint_state_{0} = (void *)0x{1:X}ULL;\n",
                       _id, reinterpret_cast<uintptr_t>(&_state));
}

std::string CLINT::genInitCode() const {
    return std::format("    _dcs_clint_tick(_clint_state_{0}, (const uint8_t*)0, (uint8_t*)0);\n", _id);
}

std::unique_ptr<Component> CLINT::clone(const std::string &n) const {
    return std::make_unique<CLINT>(n);
}

std::string CLINT::genFuncDef_seq() const {
    int id = _id;
    std::string code;
    code += std::format("// CLINT\n");
    code += "extern void _dcs_clint_tick(void *, const uint8_t *, uint8_t *);\n\n";
    code += std::format("static void {}() {{\n", funcName_seq());
    // 读取 Wishbone 输入
    code += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + std::to_string(inputs()[I_ADR]->netId()) + "], 4);\n";
    code += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + std::to_string(inputs()[I_WDAT]->netId()) + "], 4);\n";
    code += "    uint8_t _we = 0; dcs_memcpy(&_we, _w[" + std::to_string(inputs()[I_WE]->netId()) + "], 1);\n";
    code += "    uint8_t _sel = 0; dcs_memcpy(&_sel, _w[" + std::to_string(inputs()[I_SEL]->netId()) + "], 1);\n";
    code += "    uint8_t _stb = 0; dcs_memcpy(&_stb, _w[" + std::to_string(inputs()[I_STB]->netId()) + "], 1);\n";
    code += "    uint8_t _cyc = 0; dcs_memcpy(&_cyc, _w[" + std::to_string(inputs()[I_CYC]->netId()) + "], 1);\n";
    code += "    uint8_t _clk = 0; dcs_memcpy(&_clk, _w[" + std::to_string(inputs()[I_CLK]->netId()) + "], 1);\n";
    code += "    uint8_t _in[" + std::to_string(I_COUNT) + "][16]; dcs_memset(_in, 0, sizeof(_in));\n";
    code += "    dcs_memcpy(_in[0], &_adr, 4); dcs_memcpy(_in[1], &_wdat, 4);\n";
    code += "    dcs_memcpy(_in[2], &_we, 1); dcs_memcpy(_in[3], &_sel, 1);\n";
    code += "    dcs_memcpy(_in[4], &_stb, 1); dcs_memcpy(_in[5], &_cyc, 1);\n";
    code += "    dcs_memcpy(_in[6], &_clk, 1);\n";
    code += "    uint8_t _out[4][16]; dcs_memset(_out, 0, sizeof(_out));\n";
    code += std::format("    _dcs_clint_tick(_clint_state_{0}, (const uint8_t*)_in, (uint8_t*)_out);\n", id);
    code += "    {\n";
    code += "        uint32_t _dout = 0; dcs_memcpy(&_dout, _out[0], 4);\n";
    code += "        uint8_t _ack = 0; dcs_memcpy(&_ack, _out[1], 1);\n";
    code += "        uint8_t _mti = 0; dcs_memcpy(&_mti, _out[2], 1);\n";
    code += "        uint8_t _msi = 0; dcs_memcpy(&_msi, _out[3], 1);\n";
    for (int i = 0; i < 4; i++) {
        const char *vars[] = {"_dout", "_ack", "_mti", "_msi"};
        int wids[] = {32, 1, 1, 1};
        code += "        " + genOutputWrite(i, vars[i], wids[i]) + "\n";
    }
    code += "    }\n}\n";
    return code;
}

// ============================================================
void CLINT::clintTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs) {
    auto &s = *static_cast<ClintState *>(state_ptr);

    // init 调用时 inputs=nullptr，仅初始化 prev_clk 避免首拍误判上升沿
    if (!inputs) {
        s.prev_clk = false;
        // 清零输出
        if (outputs) {
            std::memset(outputs, 0, 4 * SLOT);
        }
        return;
    }

    uint32_t adr; std::memcpy(&adr, inputs + I_ADR * SLOT, 4);
    uint32_t wdat; std::memcpy(&wdat, inputs + I_WDAT * SLOT, 4);
    bool we = (inputs[I_WE * SLOT] & 1) != 0;
    uint8_t sel = inputs[I_SEL * SLOT];
    bool stb = (inputs[I_STB * SLOT] & 1) != 0;
    bool cyc = (inputs[I_CYC * SLOT] & 1) != 0;
    bool clk = (inputs[I_CLK * SLOT] & 1) != 0;

    bool rising = !s.prev_clk && clk;
    s.prev_clk = clk;

    // 每个时钟上升沿 mtime 自增
    if (rising) {
        s.mtime++;
    }

    // mtime 和 msip 是组合逻辑（随时可读），但 mtimecmp 写入是时序的
    bool stb_active = stb && cyc;
    bool stb_rise = stb_active && !s.prev_stb;
    s.prev_stb = stb_active;

    uint32_t dout = 0;
    bool ack = false;

    // 读操作总是可用（组合逻辑）
    if (stb_active && !we) {
        switch (adr & 0xFFFC) {
        case OFF_MSIP:
            dout = s.msip;
            break;
        case OFF_MTIMECMP:
            // mtimecmp 低 32 位
            dout = (uint32_t)(s.mtimecmp & 0xFFFFFFFFULL);
            break;
        case OFF_MTIMECMP + 4:
            // mtimecmp 高 32 位
            dout = (uint32_t)((s.mtimecmp >> 32) & 0xFFFFFFFFULL);
            break;
        case OFF_MTIME:
            // mtime 低 32 位
            dout = (uint32_t)(s.mtime & 0xFFFFFFFFULL);
            break;
        case OFF_MTIME + 4:
            // mtime 高 32 位
            dout = (uint32_t)((s.mtime >> 32) & 0xFFFFFFFFULL);
            break;
        default:
            break;
        }
        ack = true;
    }

    // 写操作：只在一次事务的第一次 stb 有效时执行，避免主设备保持 stb 期间重复写
    if (stb_rise && we) {
        switch (adr & 0xFFFC) {
        case OFF_MSIP:
            if (sel & 0x01) { s.msip = (s.msip & 0xFFFFFF00u) | (wdat & 0xFFu); }
            if (sel & 0x02) { s.msip = (s.msip & 0xFFFF00FFu) | (wdat & 0xFF00u); }
            if (sel & 0x04) { s.msip = (s.msip & 0xFF00FFFFu) | (wdat & 0xFF0000u); }
            if (sel & 0x08) { s.msip = (s.msip & 0x00FFFFFFu) | (wdat & 0xFF000000u); }
            break;
        case OFF_MTIMECMP: {
            // 写 mtimecmp 低 32 位
            uint64_t hi = s.mtimecmp & 0xFFFFFFFF00000000ULL;
            uint64_t lo = wdat;
            if (sel & 0x01) lo = (lo & 0xFFFFFF00ULL) | (wdat & 0xFFULL);
            if (sel & 0x02) lo = (lo & 0xFFFF00FFULL) | ((wdat >> 8) & 0xFFULL) << 8;
            if (sel & 0x04) lo = (lo & 0xFF00FFFFULL) | ((wdat >> 16) & 0xFFULL) << 16;
            if (sel & 0x08) lo = (lo & 0x00FFFFFFULL) | ((wdat >> 24) & 0xFFULL) << 24;
            s.mtimecmp = hi | lo;
            break;
        }
        case OFF_MTIMECMP + 4: {
            uint64_t lo = s.mtimecmp & 0xFFFFFFFFULL;
            uint64_t hi = (uint64_t)wdat << 32;
            if (sel & 0x01) hi = (hi & 0xFFFFFF00FFFFFFFFULL) | ((uint64_t)(wdat & 0xFFULL) << 32);
            if (sel & 0x02) hi = (hi & 0xFFFF00FFFFFFFFFFULL) | ((uint64_t)((wdat >> 8) & 0xFFULL) << 40);
            if (sel & 0x04) hi = (hi & 0xFF00FFFFFFFFFFFFULL) | ((uint64_t)((wdat >> 16) & 0xFFULL) << 48);
            if (sel & 0x08) hi = (hi & 0x00FFFFFFFFFFFFFFULL) | ((uint64_t)((wdat >> 24) & 0xFFULL) << 56);
            s.mtimecmp = hi | lo;
            break;
        }
        default:
            break;
        }
        ack = true;
    }

    // 中断信号: 组合逻辑
    bool mti = (s.mtime >= s.mtimecmp);
    bool msi = (s.msip & 1) != 0;

    // 打包输出
    std::memset(outputs, 0, 4 * SLOT);
    std::memcpy(outputs + O_DAT * SLOT, &dout, 4);
    outputs[O_ACK * SLOT] = ack ? (uint8_t)1 : (uint8_t)0;
    outputs[O_MTI * SLOT] = mti ? (uint8_t)1 : (uint8_t)0;
    outputs[O_MSI * SLOT] = msi ? (uint8_t)1 : (uint8_t)0;
}

} // namespace dsc
