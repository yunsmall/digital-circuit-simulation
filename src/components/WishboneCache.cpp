#include "dcs/components/WishboneCache.h"

#include <cstring>
#include <format>

namespace dsc {

namespace {
constexpr int SLOT = 16;
// 上游（面对 CPU）
enum UpIdx { U_ADR = 0, U_DAT, U_WE, U_SEL, U_STB, U_CYC, U_COUNT };
// 下游（面对存储器）
enum DnIdx { D_DAT_I = 0, D_ACK_I, D_COUNT };
enum OutIdx { O_DAT = 0, O_ACK, O_DADR, O_DDAT, O_DWE, O_DSEL, O_DSTB, O_DCYC, O_COUNT };

enum Fill { F_IDLE = 0, F_REQ, F_WAIT, F_DONE };
} // namespace

WishboneCache::WishboneCache(const std::string &name, int lines)
    : SequentialComponent(name, "wb_cache"), _lines(lines) {
    if (_lines < 2)
        _lines = 2;
    if (_lines > 256)
        _lines = 256;
    setParam("lines", std::to_string(_lines));
    addInput("wb_adr_i", 32);
    addInput("wb_dat_i", 32);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", 4);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("wb_dat_o_i", 32); // 来自存储器的读数据
    addInput("wb_ack_o_i", 1);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
    addOutput("wb_adr_o", 32, false, true);
    addOutput("wb_dat_i_o", 32, false, true);
    addOutput("wb_we_o", 1, false, true);
    addOutput("wb_sel_o", 4, false, true);
    addOutput("wb_stb_o", 1, false, true);
    addOutput("wb_cyc_o", 1, false, true);
}

WishboneCache::~WishboneCache() = default;

bool WishboneCache::prepare(std::string &) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> WishboneCache::extraJitSymbols() const {
    return {{"_dcs_cache_tick", reinterpret_cast<void *>(&WishboneCache::cacheTick)},
            {"_dcs_cache_reset", reinterpret_cast<void *>(&WishboneCache::cacheReset)}};
}

void WishboneCache::cacheReset(void *state_ptr, int lines) {
    auto &s = *static_cast<CacheState *>(state_ptr);
    std::memset(&s, 0, sizeof(s));
    s.num_lines = lines;
}

std::string WishboneCache::genStateDecl() const {
    return std::format("static void *_cache_state_{} = (void*)0x{:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_state));
}

std::string WishboneCache::genInitCode() const {
    return std::format("    _dcs_cache_reset(_cache_state_{}, {});\n", _id, _lines);
}

std::string WishboneCache::genFuncDef_seq() const {
    std::string code;
    code += "extern void _dcs_cache_tick(void *, const uint8_t *, uint8_t *);\n";
    code += "extern void _dcs_cache_reset(void *, int);\n\n";
    code += std::format("static void {}(void) {{\n", funcName_seq());
    code += std::format("    uint8_t _in[{}][{}];\n", (int) U_COUNT + (int) D_COUNT, SLOT);
    int idx = 0;
    for (int i = 0; i < U_COUNT; i++, idx++) {
        int w = (i == U_ADR || i == U_DAT) ? 32 : (i == U_SEL ? 4 : 1);
        code += std::format("    {{ uint64_t _v=0; dcs_memcpy(&_v,_w[{}],{}); uint8_t _b[8]; dcs_memcpy(_b,&_v,8); "
                            "dcs_memcpy(_in[{}],_b,{}); }}\n",
                            inputs()[i]->netId(), byte_count(w), idx, byte_count(w));
    }
    for (int i = 0; i < D_COUNT; i++, idx++) {
        int w = (i == D_DAT_I) ? 32 : 1;
        code += std::format("    {{ uint64_t _v=0; dcs_memcpy(&_v,_w[{}],{}); uint8_t _b[8]; dcs_memcpy(_b,&_v,8); "
                            "dcs_memcpy(_in[{}],_b,{}); }}\n",
                            inputs()[U_COUNT + i]->netId(), byte_count(w), idx, byte_count(w));
    }
    code += std::format("    uint8_t _out[{}][{}];\n", (int) O_COUNT, SLOT);
    code += std::format("    _dcs_cache_tick(_cache_state_{0}, (const uint8_t*)_in, (uint8_t*)_out);\n", _id);
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_out[0],4); " + genOutputWrite(O_DAT, "_d", 32) + " }\n";
    code += "    { uint8_t _a=0; dcs_memcpy(&_a,_out[1],1); " + genOutputWrite(O_ACK, "_a", 1) + " }\n";
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_out[2],4); " + genOutputWrite(O_DADR, "_d", 32) + " }\n";
    code += "    { uint32_t _d=0; dcs_memcpy(&_d,_out[3],4); " + genOutputWrite(O_DDAT, "_d", 32) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_out[4],1); " + genOutputWrite(O_DWE, "_v", 1) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_out[5],1); " + genOutputWrite(O_DSEL, "_v", 4) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_out[6],1); " + genOutputWrite(O_DSTB, "_v", 1) + " }\n";
    code += "    { uint8_t _v=0; dcs_memcpy(&_v,_out[7],1); " + genOutputWrite(O_DCYC, "_v", 1) + " }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> WishboneCache::clone(const std::string &new_name) const {
    auto p = std::make_unique<WishboneCache>(new_name, _lines);
    p->_state = _state;
    return p;
}

void WishboneCache::cacheTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs) {
    auto &s = *static_cast<CacheState *>(state_ptr);

    std::memcpy(&s.up_adr, inputs + U_ADR * SLOT, 4);
    std::memcpy(&s.up_dat, inputs + U_DAT * SLOT, 4);
    s.up_we = (inputs[U_WE * SLOT] & 1) != 0;
    s.up_sel = inputs[U_SEL * SLOT];
    s.up_stb = (inputs[U_STB * SLOT] & 1) != 0;
    s.up_cyc = (inputs[U_CYC * SLOT] & 1) != 0;
    uint32_t dn_dat_i = 0;
    std::memcpy(&dn_dat_i, inputs + (U_COUNT + D_DAT_I) * SLOT, 4);
    bool dn_ack_i = (inputs[(U_COUNT + D_ACK_I) * SLOT] & 1) != 0;

    bool up_active = s.up_stb && s.up_cyc;
    // 只有 stb 的上升沿才代表一个新请求；主设备会保持 stb 直到看到 ack，
    // 期间若重复接收请求，下游会看到同一笔写被发起多次
    bool up_rise = up_active && !s.prev_up_stb;
    uint32_t dout = 0;
    bool up_ack = false;

    uint32_t line_idx = (s.up_adr >> 4) % (uint32_t) s.num_lines;
    uint32_t tag = s.up_adr >> 4;

    switch (s.fill_state) {
    case F_IDLE:
        // 还有未完成的写事务时不接受新请求，否则会把它覆盖掉导致上游挂死
        if (up_rise && !s.pending_we) {
            if (!s.up_we) {
                CacheLine &l = s.line[line_idx];
                if (l.valid && l.tag == tag) {
                    dout = l.data[(s.up_adr >> 2) & 3];
                    up_ack = true;
                    s.hits++;
                } else {
                    // 未命中：整行填充，先把请求记下来
                    s.misses++;
                    s.pending_adr = s.up_adr;
                    s.pending_we = false;
                    s.fill_addr = s.up_adr & ~0xFu;
                    s.fill_line = line_idx;
                    s.fill_word = 0;
                    s.fill_state = F_REQ;
                }
            } else {
                // 写直通且不分配：不必先填充整行，直接把写送到下游
                CacheLine &l = s.line[line_idx];
                if (l.valid && l.tag == tag) {
                    s.hits++;
                    // 只更新被选中的字节，否则字节写会把同字其它字节一起改掉
                    uint8_t *dst = reinterpret_cast<uint8_t *>(&l.data[(s.up_adr >> 2) & 3]);
                    const uint8_t *src = reinterpret_cast<const uint8_t *>(&s.up_dat);
                    for (int b = 0; b < 4; b++)
                        if (s.up_sel & (1u << b))
                            dst[b] = src[b];
                } else {
                    s.misses++;
                }
                s.pending_adr = s.up_adr;
                s.pending_wdata = s.up_dat;
                s.pending_sel = s.up_sel; // 锁存，等 ack 期间上游可能撤掉 sel
                s.fill_line = line_idx;
                s.pending_we = true;
                s.fill_word = 1; // 标记"写已进入发送阶段"
            }
        }
        break;

    case F_REQ:
    case F_WAIT: {
        uint32_t addr = s.fill_addr + (uint32_t) s.fill_word * 4;
        if (s.fill_state == F_REQ) {
            s.dn_adr = addr;
            s.dn_we = false;
            s.dn_stb = s.dn_cyc = true;
            s.fill_state = F_WAIT;
        } else if (dn_ack_i) {
            s.line[s.fill_line].data[s.fill_word] = dn_dat_i;
            s.dn_stb = s.dn_cyc = false;
            if (++s.fill_word >= LINE_WORDS) {
                s.line[s.fill_line].valid = true;
                s.line[s.fill_line].tag = s.fill_addr >> 4;
                s.fill_state = F_IDLE;
                dout = s.line[s.fill_line].data[(s.pending_adr >> 2) & 3];
                up_ack = true;
            } else {
                s.fill_state = F_REQ;
            }
        }
        break;
    }
    default: break;
    }

    // 写直通：把写请求送到下游并等应答，不做写分配
    if (s.pending_we && s.fill_word == 1) {
        s.dn_adr = s.pending_adr;
        s.dn_dat = s.pending_wdata;
        s.dn_we = true;
        s.dn_stb = s.dn_cyc = true;
        s.fill_word = 2; // 已发出，等 ack
    } else if (s.pending_we && s.fill_word == 2 && dn_ack_i) {
        s.dn_stb = s.dn_cyc = false;
        s.pending_we = false;
        s.fill_word = 0;
        up_ack = true; // 命中的行在接请求时就地更新过了
    }

    // 主设备会保持 stb 直到看见 ack，所以应答和数据必须一直有效到事务结束
    if (up_active && s.up_hold_ack) {
        dout = s.up_latched;
        up_ack = true;
    }
    if (up_ack) {
        s.up_latched = dout;
        s.up_hold_ack = true;
    }
    if (!up_active)
        s.up_hold_ack = false;
    s.prev_up_stb = up_active;

    std::memset(outputs, 0, O_COUNT * SLOT);
    std::memcpy(outputs + O_DAT * SLOT, &dout, 4);
    outputs[O_ACK * SLOT] = up_ack ? 1 : 0;
    std::memcpy(outputs + O_DADR * SLOT, &s.dn_adr, 4);
    std::memcpy(outputs + O_DDAT * SLOT, &s.dn_dat, 4);
    outputs[O_DWE * SLOT] = s.dn_we ? 1 : 0;
    outputs[O_DSEL * SLOT] = s.dn_we ? s.pending_sel : 0x0F; // 写用锁存的字节使能，读整字
    outputs[O_DSTB * SLOT] = s.dn_stb ? 1 : 0;
    outputs[O_DCYC * SLOT] = s.dn_cyc ? 1 : 0;
}

} // namespace dsc
