#include "dcs/components/PLIC.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_SRC, I_COUNT };
enum OutIdx { O_DAT = 0, O_ACK, O_IRQ, O_COUNT };

constexpr uint32_t OFF_PRIORITY = 0x000000;
constexpr uint32_t OFF_PENDING = 0x001000;
constexpr uint32_t OFF_ENABLE = 0x002000;
constexpr uint32_t OFF_THRESHOLD = 0x200000;
constexpr uint32_t OFF_CLAIM = 0x200004;
} // namespace

PLIC::PLIC(const std::string &name, int num_sources) : SequentialComponent(name, "plic"), _num_sources(num_sources) {
    if (_num_sources < 1)
        _num_sources = 1;
    if (_num_sources > MAX_SOURCES)
        _num_sources = MAX_SOURCES;
    setParam("num_sources", std::to_string(_num_sources));
    addInput("clk", 1);
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("irq_src", _num_sources); // 每个 bit 一个中断源，bit0 对应源 1
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("irq", 1, false, true);
}

PLIC::~PLIC() = default;

bool PLIC::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> PLIC::extraJitSymbols() const {
    return {{"_dcs_plic_tick", reinterpret_cast<void *>(&PLIC::plicTick)},
            {"_dcs_plic_reset", reinterpret_cast<void *>(&PLIC::plicReset)}};
}

void PLIC::plicReset(void *state_ptr, int num_sources) {
    auto &s = *static_cast<PlicState *>(state_ptr);
    std::memset(&s, 0, sizeof(s)); // 复位后所有源、使能、阈值全部清空
    s.num_sources = (uint32_t) num_sources;
}

std::string PLIC::genStateDecl() const {
    return std::format("static void *_plic_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string PLIC::genInitCode() const {
    return std::format("    _dcs_plic_reset(_plic_state_{}, {});\n", _id, _num_sources);
}

std::string PLIC::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_plic_tick(void *, const uint8_t *, uint8_t *);\n";
    code += "extern void _dcs_plic_reset(void *, int);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += std::format("    uint8_t _in[{}][{}];\n", (int) I_COUNT, SLOT);
    code += "    dcs_memset(_in, 0, sizeof(_in));\n"; // 位宽不足 4 字节的输入会留高位垃圾
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_w[" + std::to_string(inputs()[I_CLK]->netId()) + "],1); dcs_memcpy(_in[0],&_v,1); }\n";
    for (int i = I_ADR; i < I_COUNT; i++) {
        int w = (i == I_ADR || i == I_DAT) ? 32 : (i == I_SEL ? 4 : (i == I_SRC ? _num_sources : 1));
        code += std::format("    {{ uint64_t _v=0; dcs_memcpy(&_v,_w[{}],{}); uint8_t _b[8]; dcs_memcpy(_b,&_v,8); "
                            "dcs_memcpy(_in[{}],_b,{}); }}\n",
                            inputs()[i]->netId(), byte_count(w), i, byte_count(w));
    }
    code += std::format("    uint8_t _out[{}][{}];\n", (int) O_COUNT, SLOT);
    code += std::format("    _dcs_plic_tick(_plic_state_{0}, (const uint8_t*)_in, (uint8_t*)_out);\n", _id);
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_out[0],4); " + genOutputWrite(O_DAT, "_d", 32) + " }\n";
    code += "    { uint8_t _a=0; dcs_memcpy(&_a,_out[1],1); " + genOutputWrite(O_ACK, "_a", 1) + " }\n";
    code += "    { uint8_t _i=0; dcs_memcpy(&_i,_out[2],1); " + genOutputWrite(O_IRQ, "_i", 1) + " }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> PLIC::clone(const std::string &new_name) const {
    auto p = std::make_unique<PLIC>(new_name, _num_sources);
    p->_state = _state;
    return p;
}

void PLIC::plicTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs) {
    auto &s = *static_cast<PlicState *>(state_ptr);

    uint32_t adr = 0, wdat = 0;
    std::memcpy(&adr, inputs + I_ADR * SLOT, 4);
    std::memcpy(&wdat, inputs + I_DAT * SLOT, 4);
    bool we = (inputs[I_WE * SLOT] & 1) != 0;
    bool stb = (inputs[I_STB * SLOT] & 1) != 0;
    bool cyc = (inputs[I_CYC * SLOT] & 1) != 0;
    uint32_t src = 0;
    std::memcpy(&src, inputs + I_SRC * SLOT, 4);

    bool active = stb && cyc;
    bool stb_rise = active && !s.prev_stb;
    s.prev_stb = active;

    // irq_src 的 bit0 对应源 1，而 pending/enable 用 bit(id) 表示源 id，这里统一左移一位
    uint32_t arrived = (src << 1) & ~s.in_service;
    s.pending |= arrived;

    uint32_t dout = 0;
    bool ack = false;

    if (active && !stb_rise) {
        dout = s.latched_dout; // 事务进行中保持上一次的应答数据
        ack = true;
    } else if (stb_rise) {
        const uint32_t off = adr & 0x00FFFFFFu;
        if (off >= OFF_PRIORITY && off < OFF_PRIORITY + 4 * (s.num_sources + 1)) {
            uint32_t id = (off - OFF_PRIORITY) / 4;
            if (!we) {
                dout = s.priority[id];
            } else if (id >= 1) {
                s.priority[id] = wdat & 7u; // PLIC 优先级只有 3 位
            }
            ack = true;
        } else if (off >= OFF_PENDING && off < OFF_PENDING + 4 * (s.num_sources + 1)) {
            uint32_t id = (off - OFF_PENDING) / 4;
            if (!we && id <= s.num_sources)
                dout = (s.pending >> id) & 1u;
            ack = true;
        } else if (off >= OFF_ENABLE && off < OFF_ENABLE + 4 * (s.num_sources + 1)) {
            uint32_t id = (off - OFF_ENABLE) / 4;
            if (!we) {
                dout = (s.enable >> id) & 1u;
            } else {
                if (wdat & 1u)
                    s.enable |= (1u << id);
                else
                    s.enable &= ~(1u << id);
            }
            ack = true;
        } else if (off == OFF_THRESHOLD) {
            if (!we)
                dout = s.threshold;
            else
                s.threshold = wdat & 7u;
            ack = true;
        } else if (off == OFF_CLAIM) {
            if (!we) {
                // claim：取使能且待处理中优先级最高者，同优先级取源号小的
                uint32_t best = 0, best_prio = s.threshold;
                uint32_t cand = s.pending & s.enable;
                for (uint32_t id = 1; id <= s.num_sources; ++id) {
                    if (!(cand & (1u << id)))
                        continue;
                    uint32_t pr = s.priority[id];
                    if (pr > best_prio) { // 标准 PLIC 要求严格大于 threshold
                        best_prio = pr;
                        best = (uint32_t) id;
                    }
                }
                if (best) {
                    s.pending &= ~(1u << best);
                    s.in_service |= (1u << best);
                }
                dout = best;
            } else {
                // complete：源号写回后允许它再次触发
                if (wdat <= s.num_sources)
                    s.in_service &= ~(1u << wdat);
            }
            ack = true;
        } else if (off == OFF_CLAIM + 4) {
            // 标准 PLIC 的 claim/complete 在 0x200004，这里保持兼容
            if (!we) {
                uint32_t best = 0, best_prio = s.threshold;
                uint32_t cand = s.pending & s.enable;
                for (uint32_t id = 1; id <= s.num_sources; ++id) {
                    if (!(cand & (1u << id)))
                        continue;
                    uint32_t pr = s.priority[id];
                    if (pr > best_prio) { // 标准 PLIC 要求严格大于 threshold
                        best_prio = pr;
                        best = (uint32_t) id;
                    }
                }
                if (best) {
                    s.pending &= ~(1u << best);
                    s.in_service |= (1u << best);
                }
                dout = best;
            } else if (wdat <= s.num_sources) {
                s.in_service &= ~(1u << wdat);
            }
            ack = true;
        } else {
            ack = true; // 未实现的偏移按 0 读、写忽略，但不挂死总线
        }
    }

    if (stb_rise)
        s.latched_dout = dout;
    if (!active)
        s.latched_dout = 0;

    // 中断输出：使能、待处理、优先级高于阈值且当前不在服务中
    bool irq = false;
    uint32_t cand = s.pending & s.enable;
    for (uint32_t id = 1; id <= s.num_sources; ++id)
        if ((cand & (1u << id)) && s.priority[id] > s.threshold) {
            irq = true;
            break;
        }

    std::memset(outputs, 0, O_COUNT * SLOT);
    std::memcpy(outputs + O_DAT * SLOT, &dout, 4);
    outputs[O_ACK * SLOT] = ack ? 1 : 0;
    outputs[O_IRQ * SLOT] = irq ? 1 : 0;
}

} // namespace dsc
