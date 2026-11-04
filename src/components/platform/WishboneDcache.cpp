// WishboneDcache — 写直通 + 总线监听的数据缓存
// 命中路径全部展开在组合阶段（1 拍应答），未命中/写直通由时序阶段推进状态机，
// 没有跨 JIT 回调。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/WishboneDcache.h"

#include <charconv>
#include <format>
#include <stdexcept>

namespace dsc {

namespace {
// 上游（面对 CPU）
enum UpIdx { U_ADR = 0, U_DAT, U_WE, U_SEL, U_STB, U_CYC };
// 下游（面对互连）
enum DnIdx { D_DAT_I = 6, D_ACK_I = 7 };
// 总线监听（互连的 RAM 从设备侧）
enum SnIdx { S_ADR = 8, S_WE = 9, S_STB = 10, S_CYC = 11 };
// 输出
enum OutIdx { O_DAT = 0, O_ACK, O_DADR, O_DDAT, O_DWE, O_DSEL, O_DSTB, O_DCYC };
} // namespace

WishboneDcache::WishboneDcache(const std::string &name, int lines, uint32_t base, uint32_t size) :
    Component(name, "wb_dcache"), _lines(lines), _base(base), _size(size) {
    // 索引用位与算，行数必须是 2 的幂；上限挡一下，避免生成的静态数组过大
    if (lines < 2 || (lines & (lines - 1)) != 0)
        throw std::invalid_argument("缓存行数必须是 2 的幂且至少为 2");
    if (lines > 8192)
        throw std::invalid_argument("缓存行数不能超过 8192");
    if (size == 0)
        throw std::invalid_argument("可缓存地址段大小不能为 0");
    setParam("lines", std::to_string(lines));
    setParam("base", std::to_string(base));
    setParam("size", std::to_string(size));

    // 上游：CPU 侧从端口
    addInput("s_adr_i", 32);
    addInput("s_dat_i", 32);
    addInput("s_we_i", 1);
    addInput("s_sel_i", 4);
    addInput("s_stb_i", 1);
    addInput("s_cyc_i", 1);
    // 下游：互连主端口
    addInput("m_dat_i", 32);
    addInput("m_ack_i", 1);
    // 监听：互连 RAM 从设备侧总线
    addInput("sn_adr_i", 32);
    addInput("sn_we_i", 1);
    addInput("sn_stb_i", 1);
    addInput("sn_cyc_i", 1);

    addOutput("s_dat_o", 32);
    addOutput("s_ack_o", 1);
    addOutput("m_adr_o", 32);
    addOutput("m_dat_o", 32);
    addOutput("m_we_o", 1);
    addOutput("m_sel_o", 4);
    addOutput("m_stb_o", 1);
    addOutput("m_cyc_o", 1);
}

WishboneDcache::~WishboneDcache() = default;

bool WishboneDcache::prepare(Context &, std::string &) {
    _prepared = true;
    return true;
}

std::string WishboneDcache::genStateDecl() const {
    return std::format(
            "#define _DC_LINES_{0} {1}u\n"
            "#define _DC_BASE_{0} 0x{2:X}u\n"
            "#define _DC_SIZE_{0} 0x{3:X}u\n"
            // tag 只存行号以上的地址位，v 的每一位对应行内一个字
            "static uint32_t _dc_tag_{0}[{1}];\n"
            "static uint8_t  _dc_data_{0}[{1}][16];\n"
            "static uint8_t  _dc_v_{0}[{1}];\n"
            // busy: 0 空闲, 1 读填充, 2 写直通
            "static uint8_t  _dc_busy_{0}, _dc_rsp_{0}, _dc_done_{0}, _dc_snprev_{0}, _dc_pact_{0};\n"
            "static uint32_t _dc_padr_{0}, _dc_pdat_{0}, _dc_rdat_{0};\n"
            "static uint8_t  _dc_pwe_{0}, _dc_psel_{0};\n"
            // 自己的写直通也会出现在监听总线上，用它把这条回波认出来
            "static uint8_t  _dc_own_{0};\n"
            "static uint32_t _dc_ownadr_{0};\n"
            "static uint64_t *_dc_stats_{0} = (uint64_t *)0x{4:X}ULL;\n",
            _id, _lines, _base, _size, reinterpret_cast<uintptr_t>(&_stats));
}

std::string WishboneDcache::genInitCode() const {
    return std::format(
            "    dcs_memset(_dc_tag_{0}, 0, sizeof(_dc_tag_{0}));\n"
            "    dcs_memset(_dc_data_{0}, 0, sizeof(_dc_data_{0}));\n"
            "    dcs_memset(_dc_v_{0}, 0, sizeof(_dc_v_{0}));\n"
            "    _dc_busy_{0} = 0; _dc_rsp_{0} = 0; _dc_done_{0} = 0; _dc_snprev_{0} = 0; _dc_pact_{0} = 0;\n"
            "    _dc_own_{0} = 0; _dc_ownadr_{0} = 0;\n"
            "    _dc_stats_{0}[0] = 0; _dc_stats_{0}[1] = 0; _dc_stats_{0}[2] = 0;\n"
            "    _dc_padr_{0} = 0; _dc_pdat_{0} = 0; _dc_rdat_{0} = 0; _dc_pwe_{0} = 0; _dc_psel_{0} = 0;\n",
            _id);
}

std::string WishboneDcache::genFuncDef_comb() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    std::string c;
    c += std::format("// WishboneDcache {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_comb());
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(U_ADR) + "], 4);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(U_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(U_WE) + "][0] & 1;\n";
    c += "    uint8_t _sel = _w[" + nid(U_SEL) + "][0] & 0x0F;\n";
    c += "    uint8_t _stb = _w[" + nid(U_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(U_CYC) + "][0] & 1;\n";
    c += "    uint32_t _mdat_i = 0; dcs_memcpy(&_mdat_i, _w[" + nid(D_DAT_I) + "], 4);\n";
    c += "    uint8_t _mack = _w[" + nid(D_ACK_I) + "][0] & 1;\n";
    c += std::format(
            "    const uint8_t _active = _stb & _cyc;\n"
            // 段外地址（MMIO）一律旁路；无符号回绕比较，段参数在生成期折叠成常量
            "    const uint8_t _ce = (uint8_t)((_adr - _DC_BASE_{0}) < _DC_SIZE_{0});\n"
            "    const uint32_t _li = (_adr >> 4) & (_DC_LINES_{0} - 1u);\n"
            "    const uint32_t _wi = (_adr >> 2) & 3u;\n"
            "    const uint8_t _hit = (uint8_t)(_dc_tag_{0}[_li] == (_adr >> 4)) & "
            "(uint8_t)((_dc_v_{0}[_li] >> _wi) & 1u);\n",
            _id);
    // CPU 侧应答：命中读直接给数据；写和填充的应答由时序阶段置 _dc_rsp 后由这里保持
    c += std::format(
            "    uint32_t _dout = 0; uint8_t _ack = 0;\n"
            "    if (_active) {{\n"
            "        if (!_we) {{\n"
            "            if (_ce && _hit) {{\n"
            "                dcs_memcpy(&_dout, &_dc_data_{0}[_li][_wi * 4u], 4);\n"
            "                _ack = 1;\n"
            "            }} else if (_dc_rsp_{0}) {{\n"
            "                _dout = _dc_rdat_{0}; _ack = 1;\n"
            "            }} else if (!_ce) {{\n"
            // MMIO 读：把从设备的应答原样回给 CPU
            "                _dout = _mdat_i; _ack = _mack;\n"
            "            }}\n"
            "        }} else {{\n"
            "            _ack = _ce ? _dc_rsp_{0} : _mack;\n"
            "        }}\n"
            "    }}\n",
            _id);
    // 主端口：空闲时由本拍请求直接发起事务（省掉等时序阶段再启动的一拍），
    // 在途事务用锁存值保持；应答未被 CPU 领取前不再发新事务
    c += std::format(
            "    uint32_t _madr = 0, _mdat = 0;\n"
            "    uint8_t _mwe = 0, _msel = 0, _mstb = 0, _mcyc = 0;\n"
            "    if (_dc_busy_{0}) {{\n"
            "        _madr = _dc_padr_{0}; _mdat = _dc_pdat_{0}; _mwe = _dc_pwe_{0}; _msel = _dc_psel_{0};\n"
            "        _mstb = 1; _mcyc = 1;\n"
            "    }} else if (_active && !_dc_rsp_{0}) {{\n"
            "        if (_ce) {{\n"
            // 读命中不需要总线；未命中和写都要走主端口
            "            if (_we || !_hit) {{\n"
            "                _madr = _adr & ~3u; _mdat = _wdat; _mwe = _we;\n"
            "                _msel = _we ? _sel : 0x0Fu; _mstb = 1; _mcyc = 1;\n"
            "            }}\n"
            "        }} else {{\n"
            "            _madr = _adr; _mdat = _wdat; _mwe = _we; _msel = _sel; _mstb = 1; _mcyc = 1;\n"
            "        }}\n"
            "    }}\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_DADR, "_madr", 32) + "\n";
    c += "    " + genOutputWrite(O_DDAT, "_mdat", 32) + "\n";
    c += "    " + genOutputWrite(O_DWE, "_mwe", 1) + "\n";
    c += "    " + genOutputWrite(O_DSEL, "_msel", 4) + "\n";
    c += "    " + genOutputWrite(O_DSTB, "_mstb", 1) + "\n";
    c += "    " + genOutputWrite(O_DCYC, "_mcyc", 1) + "\n";
    c += "}\n";
    return c;
}

std::string WishboneDcache::genFuncDef_seq() const {
    auto nid = [&](int pin) { return std::to_string(inputs()[pin]->netId()); };
    auto oid = [&](int pin) { return std::to_string(outputs()[pin]->netId()); };
    std::string c;
    c += std::format("// WishboneDcache {} 的状态机\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    // 主端口本拍发出的请求（组合阶段刚写下的线网值）和从设备应答
    c += "    uint32_t _madr = 0; dcs_memcpy(&_madr, _w[" + oid(O_DADR) + "], 4);\n";
    c += "    uint32_t _mdat = 0; dcs_memcpy(&_mdat, _w[" + oid(O_DDAT) + "], 4);\n";
    c += "    uint8_t _mwe = _w[" + oid(O_DWE) + "][0] & 1;\n";
    c += "    uint8_t _msel = _w[" + oid(O_DSEL) + "][0] & 0x0F;\n";
    c += "    uint8_t _mstb = _w[" + oid(O_DSTB) + "][0] & 1;\n";
    c += "    uint32_t _mdat_i = 0; dcs_memcpy(&_mdat_i, _w[" + nid(D_DAT_I) + "], 4);\n";
    c += "    uint8_t _mack = _w[" + nid(D_ACK_I) + "][0] & 1;\n";
    c += "    uint8_t _stb = _w[" + nid(U_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(U_CYC) + "][0] & 1;\n";
    c += "    uint32_t _uadr = 0; dcs_memcpy(&_uadr, _w[" + nid(U_ADR) + "], 4);\n";
    c += "    uint8_t _uwe = _w[" + nid(U_WE) + "][0] & 1;\n";
    c += "    const uint8_t _active = _stb & _cyc;\n";
    // 统计：每笔上游请求各记一次命中/未命中
    c += std::format(
            "    if (_active && !_dc_pact_{0}) {{\n"
            "        _dc_stats_{0}[2]++;\n"
            "        const uint8_t _chet = (uint8_t)((_uadr - _DC_BASE_{0}) < _DC_SIZE_{0});\n"
            "        const uint32_t _hli = (_uadr >> 4) & (_DC_LINES_{0} - 1u);\n"
            "        const uint32_t _hwi = (_uadr >> 2) & 3u;\n"
            "        const uint8_t _hhit = (uint8_t)(_dc_tag_{0}[_hli] == (_uadr >> 4)) & "
            "(uint8_t)((_dc_v_{0}[_hli] >> _hwi) & 1u);\n"
            "        if (_chet && !_uwe && _hhit) _dc_stats_{0}[0]++; else _dc_stats_{0}[1]++;\n"
            "    }}\n"
            "    _dc_pact_{0} = _active;\n",
            _id);
    // 监听：互连把其它主设备的写事务转发到 RAM 从设备侧，这里按 stb 上升沿失效
    c += "    uint32_t _sn_adr = 0; dcs_memcpy(&_sn_adr, _w[" + nid(S_ADR) + "], 4);\n";
    c += "    uint8_t _sn_we = _w[" + nid(S_WE) + "][0] & 1;\n";
    c += "    uint8_t _sn_stb = _w[" + nid(S_STB) + "][0] & 1;\n";
    c += "    uint8_t _sn_cyc = _w[" + nid(S_CYC) + "][0] & 1;\n";
    c += std::format(
            "    const uint8_t _sn_act = _sn_stb & _sn_cyc;\n"
            "    const uint8_t _sn_rise = _sn_act & (uint8_t)(!_dc_snprev_{0});\n"
            "    _dc_snprev_{0} = _sn_act;\n",
            _id);
    // 启动：组合阶段本拍已经在主端口上发出可缓存事务，锁存进状态机。
    // 段外访问是纯旁路，不进状态机
    c += std::format(
            "    const uint8_t _ce = (uint8_t)((_madr - _DC_BASE_{0}) < _DC_SIZE_{0});\n"
            "    if (!_dc_busy_{0} && _mstb && _ce) {{\n"
            "        if (_mwe) {{\n",
            _id);
    // 写直通命中行就地更新。字本来无效时只有整字写才敢标有效，否则未选中的字节是旧内容
    c += std::format(
            "            const uint32_t _li = (_madr >> 4) & (_DC_LINES_{0} - 1u);\n"
            "            const uint32_t _wi = (_madr >> 2) & 3u;\n"
            "            if (_dc_tag_{0}[_li] == (_madr >> 4)) {{\n"
            "                uint8_t *_dst = &_dc_data_{0}[_li][_wi * 4u];\n"
            "                if (_msel & 1u) _dst[0] = (uint8_t)(_mdat & 0xFFu);\n"
            "                if (_msel & 2u) _dst[1] = (uint8_t)((_mdat >> 8) & 0xFFu);\n"
            "                if (_msel & 4u) _dst[2] = (uint8_t)((_mdat >> 16) & 0xFFu);\n"
            "                if (_msel & 8u) _dst[3] = (uint8_t)((_mdat >> 24) & 0xFFu);\n"
            "                if ((_dc_v_{0}[_li] & (uint8_t)(1u << _wi)) || _msel == 0x0Fu)\n"
            "                    _dc_v_{0}[_li] |= (uint8_t)(1u << _wi);\n"
            "            }}\n"
            "        }}\n"
            "        _dc_padr_{0} = _madr; _dc_pdat_{0} = _mdat; _dc_pwe_{0} = _mwe; _dc_psel_{0} = _msel;\n"
            "        _dc_busy_{0} = _mwe ? 2 : 1;\n"
            "        if (_mwe) {{ _dc_own_{0} = 1; _dc_ownadr_{0} = _madr; }}\n"
            "    }}\n",
            _id);
    // 完成：ack 高且这笔事务还没完成过。用 _dc_done 防止 ack 保持多拍时重复完成；
    // 主端口撤下 stb 后清掉，等下一笔
    c += std::format(
            "    if (!_mstb) {{ _dc_done_{0} = 0; _dc_own_{0} = 0; }}\n"
            "    else if (_ce && _mack && !_dc_done_{0}) {{\n"
            "        if (_dc_busy_{0} == 1) {{\n"
            "            const uint32_t _li = (_dc_padr_{0} >> 4) & (_DC_LINES_{0} - 1u);\n"
            "            const uint32_t _wi = (_dc_padr_{0} >> 2) & 3u;\n"
            "            const uint32_t _tag = _dc_padr_{0} >> 4;\n"
            // 换 tag 的时候旧行的有效位必须整体作废
            "            if (_dc_tag_{0}[_li] != _tag) {{ _dc_tag_{0}[_li] = _tag; _dc_v_{0}[_li] = 0; }}\n"
            "            dcs_memcpy(&_dc_data_{0}[_li][_wi * 4u], &_mdat_i, 4);\n"
            "            _dc_v_{0}[_li] |= (uint8_t)(1u << _wi);\n"
            "            _dc_rdat_{0} = _mdat_i;\n"
            "        }}\n"
            "        _dc_rsp_{0} = 1;\n"
            "        _dc_busy_{0} = 0;\n"
            "        _dc_done_{0} = 1;\n"
            "    }}\n",
            _id);
    // 监听失效放在状态机之后：这样 _dc_own 已经反映本拍启动的写直通。
    // 自己的写直通会从互连回波到监听口，它不是外部修改，行里已经是新值；
    // 用"写事务期间记住的地址"认出来，而不是用 busy 状态——回波可能正好落在
    // 完成那一拍，那时 busy 已经清了
    c += std::format(
            "    if (_sn_rise && _sn_we && !(_dc_own_{0} && _dc_ownadr_{0} == (_sn_adr & ~3u))) {{\n"
            "        const uint32_t _sli = (_sn_adr >> 4) & (_DC_LINES_{0} - 1u);\n"
            "        if (_dc_tag_{0}[_sli] == (_sn_adr >> 4)) _dc_v_{0}[_sli] = 0;\n"
            "    }}\n",
            _id);
    // 应答被上游领取（或撤销）后清掉，下一笔事务才能重新开始
    c += std::format("    if (!_active) _dc_rsp_{0} = 0;\n", _id);
    c += "}\n";
    return c;
}

std::unique_ptr<Component> WishboneDcache::clone(const std::string &new_name) const {
    return std::make_unique<WishboneDcache>(new_name, _lines, _base, _size);
}

std::vector<int> WishboneDcache::getCombinationalDeps(int out_idx) const {
    if (out_idx >= 0 && out_idx < (int) outputs().size() && outputs()[out_idx]->isTriState())
        return {};
    // 门控引脚（监听）只给时序阶段用；组合输出不依赖它们
    return {U_ADR, U_DAT, U_WE, U_SEL, U_STB, U_CYC, D_DAT_I, D_ACK_I};
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
// base/size 是 32 位无符号数，paramInt 只认 int，这里单独解析
uint32_t paramU32(const std::unordered_map<std::string, std::string> &p, const std::string &k, uint32_t d) {
    auto it = p.find(k);
    if (it == p.end())
        return d;
    unsigned long long v = 0;
    auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    return (ec == std::errc{} && ptr == it->second.data() + it->second.size()) ? (uint32_t) v : d;
}
DCS_REGISTER_COMPONENT(
    {"wb_dcache", {{"lines", "1024"}, {"base", "2147483648"}, {"size", "268435456"}},
     {"s_adr_i", "s_dat_i", "s_we_i", "s_sel_i", "s_stb_i", "s_cyc_i", "m_dat_i", "m_ack_i", "sn_adr_i", "sn_we_i",
      "sn_stb_i", "sn_cyc_i"},
     {"s_dat_o", "s_ack_o", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"},
     dsc::ComponentCategory::Memory, "写直通数据缓存：命中读当拍应答，监听总线失效其它主设备写入的行"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::WishboneDcache>(n, dsc::paramInt(p, "lines", 1024),
                                                         paramU32(p, "base", 0x80000000u),
                                                         paramU32(p, "size", 0x10000000u));
        });
} // namespace
