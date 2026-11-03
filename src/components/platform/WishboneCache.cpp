// WishboneCache — 直接映射的数据缓存，串接在 CPU 与存储器之间
// 状态机和整行填充流程全部展开在 JIT 里；命中/未命中计数器放在 C++ 成员里，
// 通过暴露的指针递增，外部统计接口不受影响。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/WishboneCache.h"

#include <format>

namespace dsc {

namespace {
// 上游（面对 CPU）
enum UpIdx { U_ADR = 0, U_DAT, U_WE, U_SEL, U_STB, U_CYC };
// 下游（面对存储器）
enum DnIdx { D_DAT_I = 0, D_ACK_I };
enum OutIdx { O_DAT = 0, O_ACK, O_DADR, O_DDAT, O_DWE, O_DSEL, O_DSTB, O_DCYC };

// 填充状态机取值
enum Fill { F_IDLE = 0, F_REQ = 1, F_WAIT = 2 };
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

bool WishboneCache::prepare(Context &, std::string &) {
    _prepared = true;
    return true;
}

std::string WishboneCache::genStateDecl() const {
    return std::format(
            "static uint32_t _cache_ld_{0}[256][4];\n"
            "static uint32_t _cache_lt_{0}[256];\n"
            "static uint8_t  _cache_lv_{0}[256];\n"
            "static uint64_t *_cache_stats_{0} = (uint64_t *)0x{1:X}ULL;\n"
            "static uint32_t _cache_latched_{0};\n"
            "static uint8_t  _cache_hold_ack_{0}, _cache_prev_stb_{0};\n"
            "static uint32_t _cache_dn_adr_{0}, _cache_dn_dat_{0};\n"
            "static uint8_t  _cache_dn_we_{0}, _cache_dn_stb_{0}, _cache_dn_cyc_{0};\n"
            "static uint8_t  _cache_fill_state_{0};\n"
            "static uint32_t _cache_fill_addr_{0}, _cache_fill_line_{0}, _cache_fill_word_{0};\n"
            "static uint32_t _cache_pend_adr_{0}, _cache_pend_wdata_{0}, _cache_pend_sel_{0};\n"
            "static uint8_t  _cache_pend_we_{0};\n",
            _id, reinterpret_cast<uintptr_t>(&_stats));
}

std::string WishboneCache::genInitCode() const {
    return std::format(
            "    dcs_memset(_cache_ld_{0}, 0, sizeof(_cache_ld_{0}));\n"
            "    dcs_memset(_cache_lt_{0}, 0, sizeof(_cache_lt_{0}));\n"
            "    dcs_memset(_cache_lv_{0}, 0, sizeof(_cache_lv_{0}));\n"
            "    _cache_stats_{0}[0] = 0; _cache_stats_{0}[1] = 0;\n"
            "    _cache_latched_{0} = 0; _cache_hold_ack_{0} = 0; _cache_prev_stb_{0} = 0;\n"
            "    _cache_dn_adr_{0} = 0; _cache_dn_dat_{0} = 0;\n"
            "    _cache_dn_we_{0} = 0; _cache_dn_stb_{0} = 0; _cache_dn_cyc_{0} = 0;\n"
            "    _cache_fill_state_{0} = 0; _cache_fill_addr_{0} = 0; _cache_fill_line_{0} = 0;\n"
            "    _cache_fill_word_{0} = 0;\n"
            "    _cache_pend_adr_{0} = 0; _cache_pend_wdata_{0} = 0; _cache_pend_sel_{0} = 0;\n"
            "    _cache_pend_we_{0} = 0;\n",
            _id);
}

std::string WishboneCache::genFuncDef_seq() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    std::string c;
    c += std::format("// WishboneCache {}\n", name());
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(U_ADR) + "], 4);\n";
    c += "    uint32_t _dat = 0; dcs_memcpy(&_dat, _w[" + nid(U_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(U_WE) + "][0] & 1;\n";
    c += "    uint8_t _sel = _w[" + nid(U_SEL) + "][0] & 0x0F;\n";
    c += "    uint8_t _stb = _w[" + nid(U_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(U_CYC) + "][0] & 1;\n";
    c += "    uint32_t _dn_dati = 0; dcs_memcpy(&_dn_dati, _w[" + nid(6 + D_DAT_I) + "], 4);\n";
    c += "    uint8_t _dn_acki = _w[" + nid(6 + D_ACK_I) + "][0] & 1;\n";
    c += "    uint32_t _dout = 0; uint8_t _up_ack = 0;\n";
    c += std::format(
            "    const bool _up_active = _stb && _cyc;\n"
            "    const bool _up_rise = _up_active && !_cache_prev_stb_{0};\n"
            "    const uint32_t _li = (_adr >> 4) % {1}u;\n"
            "    const uint32_t _tag = _adr >> 4;\n"
            "    if (_cache_fill_state_{0} == 0) {{\n"
            "        if (_up_rise && !_cache_pend_we_{0}) {{\n"
            "            if (!_we) {{\n"
            "                if (_cache_lv_{0}[_li] && _cache_lt_{0}[_li] == _tag) {{\n"
            "                    _dout = _cache_ld_{0}[_li][(_adr >> 2) & 3];\n"
            "                    _up_ack = 1;\n"
            "                    _cache_stats_{0}[0]++;\n"
            "                }} else {{\n"
            "                    _cache_stats_{0}[1]++;\n"
            "                    _cache_pend_adr_{0} = _adr;\n"
            "                    _cache_pend_we_{0} = 0;\n"
            "                    _cache_fill_addr_{0} = _adr & ~0xFu;\n"
            "                    _cache_fill_line_{0} = _li;\n"
            "                    _cache_fill_word_{0} = 0;\n"
            "                    _cache_fill_state_{0} = 1;\n"
            "                }}\n"
            "            }} else {{\n"
            "                if (_cache_lv_{0}[_li] && _cache_lt_{0}[_li] == _tag) {{\n"
            "                    _cache_stats_{0}[0]++;\n",
            _id, _lines);
    // 只更新被选中的字节，否则字节写会把同字其它字节一起改掉
    c += std::format("                    uint8_t *_dst = (uint8_t *)&_cache_ld_{0}[_li][(_adr >> 2) & 3];\n"
                     "                    const uint8_t *_src = (const uint8_t *)&_dat;\n"
                     "                    if (_sel & 1u) _dst[0] = _src[0];\n"
                     "                    if (_sel & 2u) _dst[1] = _src[1];\n"
                     "                    if (_sel & 4u) _dst[2] = _src[2];\n"
                     "                    if (_sel & 8u) _dst[3] = _src[3];\n"
                     "                }} else {{\n"
                     "                    _cache_stats_{0}[1]++;\n"
                     "                }}\n"
                     "                _cache_pend_adr_{0} = _adr;\n"
                     "                _cache_pend_wdata_{0} = _dat;\n"
                     "                _cache_pend_sel_{0} = _sel;\n" // 锁存，等 ack 期间上游可能撤掉 sel
                     "                _cache_fill_line_{0} = _li;\n"
                     "                _cache_pend_we_{0} = 1;\n"
                     "                _cache_fill_word_{0} = 1;\n" // 标记"写已进入发送阶段"
                     "            }}\n"
                     "        }}\n"
                     "    }} else {{\n"
                     "        const uint32_t _faddr = _cache_fill_addr_{0} + _cache_fill_word_{0} * 4u;\n"
                     "        if (_cache_fill_state_{0} == 1) {{\n"
                     "            _cache_dn_adr_{0} = _faddr;\n"
                     "            _cache_dn_we_{0} = 0;\n"
                     "            _cache_dn_stb_{0} = 1; _cache_dn_cyc_{0} = 1;\n"
                     "            _cache_fill_state_{0} = 2;\n"
                     "        }} else if (_dn_acki) {{\n"
                     "            _cache_ld_{0}[_cache_fill_line_{0}][_cache_fill_word_{0}] = _dn_dati;\n"
                     "            _cache_dn_stb_{0} = 0; _cache_dn_cyc_{0} = 0;\n"
                     "            if (++_cache_fill_word_{0} >= 4) {{\n"
                     "                _cache_lv_{0}[_cache_fill_line_{0}] = 1;\n"
                     "                _cache_lt_{0}[_cache_fill_line_{0}] = _cache_fill_addr_{0} >> 4;\n"
                     "                _cache_fill_state_{0} = 0;\n"
                     "                _dout = _cache_ld_{0}[_cache_fill_line_{0}][(_cache_pend_adr_{0} >> 2) & 3];\n"
                     "                _up_ack = 1;\n"
                     "            }} else {{\n"
                     "                _cache_fill_state_{0} = 1;\n"
                     "            }}\n"
                     "        }}\n"
                     "    }}\n",
                     _id);
    // 写直通：把写请求送到下游并等应答，不做写分配
    c += std::format(
            "    if (_cache_pend_we_{0} && _cache_fill_word_{0} == 1) {{\n"
            "        _cache_dn_adr_{0} = _cache_pend_adr_{0};\n"
            "        _cache_dn_dat_{0} = _cache_pend_wdata_{0};\n"
            "        _cache_dn_we_{0} = 1;\n"
            "        _cache_dn_stb_{0} = 1; _cache_dn_cyc_{0} = 1;\n"
            "        _cache_fill_word_{0} = 2;\n" // 已发出，等 ack
            "    }} else if (_cache_pend_we_{0} && _cache_fill_word_{0} == 2 && _dn_acki) {{\n"
            "        _cache_dn_stb_{0} = 0; _cache_dn_cyc_{0} = 0;\n"
            "        _cache_pend_we_{0} = 0;\n"
            "        _cache_fill_word_{0} = 0;\n"
            "        _up_ack = 1;\n" // 命中的行在接请求时就地更新过了
            "    }}\n"
            "    if (_up_active && _cache_hold_ack_{0}) {{\n"
            "        _dout = _cache_latched_{0};\n"
            "        _up_ack = 1;\n"
            "    }}\n"
            "    if (_up_ack) {{ _cache_latched_{0} = _dout; _cache_hold_ack_{0} = 1; }}\n"
            "    if (!_up_active) _cache_hold_ack_{0} = 0;\n"
            // 填充期间不改 prev_stb：否则上游在填充中拉低再拉高的 stb 会被吞掉，
            // 缓存停在填充态，新请求永远进不来
            "    if (_cache_fill_state_{0} == 0) _cache_prev_stb_{0} = _up_active;\n"
            "    const uint8_t _dsel = _cache_dn_we_{0} ? _cache_pend_sel_{0} : 0x0F;\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_up_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_DADR, "_cache_dn_adr_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_DDAT, "_cache_dn_dat_" + std::to_string(_id), 32) + "\n";
    c += "    " + genOutputWrite(O_DWE, "_cache_dn_we_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_DSEL, "_dsel", 4) + "\n";
    c += "    " + genOutputWrite(O_DSTB, "_cache_dn_stb_" + std::to_string(_id), 1) + "\n";
    c += "    " + genOutputWrite(O_DCYC, "_cache_dn_cyc_" + std::to_string(_id), 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> WishboneCache::clone(const std::string &new_name) const {
    auto p = std::make_unique<WishboneCache>(new_name, _lines);
    p->_stats[0] = _stats[0];
    p->_stats[1] = _stats[1];
    return p;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"wb_cache", {{"lines", "16"}}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "wb_dat_o_i", "wb_ack_o_i"}, {"wb_dat_o", "wb_ack_o", "wb_adr_o", "wb_dat_i_o", "wb_we_o", "wb_sel_o", "wb_stb_o", "wb_cyc_o"}, dsc::ComponentCategory::Memory, "直接映射数据缓存，串在 CPU 与存储器之间"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::WishboneCache>(n, dsc::paramInt(p, "lines", 16));
        });
} // namespace
