// WishboneInterconnect — Wishbone 地址解码器
// 路由逻辑全部展开进 JIT 生成的 C 代码：没有跨模块调用、没有结构体指针追逐，
// 地址段在编译期折叠成常量比较。这样 LLVM 后端能优化到接近手写，
// TinyCC 直译出来的也只是一串标量比较。
#include "dcs/Circuit.h"
#include "dcs/CodeGen.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/WishboneInterconnect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dsc {

// 互连卡死检测的宿主回调；实现在本文件末尾
extern "C" void dcs_wb_trace(uint32_t adr, uint32_t grant, uint32_t si, uint32_t stb, uint32_t cyc, uint32_t busy);

WishboneInterconnect::WishboneInterconnect(const std::string &name, const std::string &segments, int num_masters) :
    Component(name, "wishbone_ic") {

    setParam("segments", segments);

    // 解析地址段
    std::istringstream ss(segments);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty())
            continue;
        auto colon = token.find(':');
        if (colon == std::string::npos)
            throw std::invalid_argument(std::format("无效地址段格式: {}", token));
        uint32_t base = (uint32_t) std::stoull(token.substr(0, colon));
        uint32_t size = (uint32_t) std::stoull(token.substr(colon + 1));
        if (size == 0)
            throw std::invalid_argument("地址段大小不能为 0");
        _segments.push_back({base, size});
    }
    if (_segments.empty())
        throw std::invalid_argument("至少需要一个地址段");
    // 地址段必须互不重叠：解码生成的是一条 else-if 比较链，重叠的段只会命中靠前的那个
    for (size_t i = 0; i < _segments.size(); i++)
        for (size_t j = i + 1; j < _segments.size(); j++) {
            const uint64_t ai = _segments[i].base, ae = ai + _segments[i].size;
            const uint64_t bi = _segments[j].base, be = bi + _segments[j].size;
            if (ai < be && bi < ae)
                throw std::invalid_argument(
                        std::format("地址段重叠: 0x{:X}+0x{:X} 与 0x{:X}+0x{:X}", ai, _segments[i].size, bi,
                                    _segments[j].size));
        }

    int N = (int) _segments.size();
    // 仲裁请求位图是 32 位（_rq |= 1u << k），超过 32 个主设备会移位 UB，
    // 且第 32 个之后无法区分。这里直接拒绝，不要生成必然错的代码
    if (num_masters > 32)
        throw std::invalid_argument("num_masters 不能超过 32（仲裁请求位图是 32 位）");
    _masters = num_masters < 1 ? 1 : num_masters;
    setParam("num_masters", std::to_string(_masters));

    // ---- 主设备侧输入 ----
    for (int mi = 0; mi < _masters; mi++) {
        const std::string p = std::format("m{}", mi);
        addInput(p + "_adr_i", 32);
        addInput(p + "_dat_i", 32);
        addInput(p + "_we_i", 1);
        addInput(p + "_sel_i", 4);
        addInput(p + "_stb_i", 1);
        addInput(p + "_cyc_i", 1);
    }

    // ---- 每个从设备侧输入（ack, dat） ----
    for (int i = 0; i < N; i++) {
        addInput(std::format("s{}_dat_i", i), 32);
        addInput(std::format("s{}_ack_i", i), 1);
    }

    // ---- 主设备侧输出 ----
    for (int mi = 0; mi < _masters; mi++) {
        const std::string p = std::format("m{}", mi);
        addOutput(p + "_dat_o", 32, false, true);
        addOutput(p + "_ack_o", 1, false, true);
    }

    // ---- 每个从设备侧输出（adr, dat, we, sel, stb, cyc） ----
    for (int i = 0; i < N; i++) {
        addOutput(std::format("s{}_adr_o", i), 32, false, true);
        addOutput(std::format("s{}_dat_o", i), 32, false, true);
        addOutput(std::format("s{}_we_o", i), 1, false, true);
        addOutput(std::format("s{}_sel_o", i), 4, false, true);
        addOutput(std::format("s{}_stb_o", i), 1, false, true);
        addOutput(std::format("s{}_cyc_o", i), 1, false, true);
    }
}

WishboneInterconnect::~WishboneInterconnect() = default;

bool WishboneInterconnect::prepare(Context &, std::string & /*error*/) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> WishboneInterconnect::extraJitSymbols() const {
    // 路由逻辑全部在生成代码里展开，这里只暴露卡死检测的回调
    return {{"dcs_wb_trace", reinterpret_cast<void *>(&dcs_wb_trace)}};
}

// ============================================================
// C 代码生成
// ============================================================

std::string WishboneInterconnect::genStateDecl() const {
    return std::format(
            "static int _wbic_grant_{0}, _wbic_next_{0}, _wbic_last_{0};\n"
            "static uint8_t _wbic_busy_{0};\n"
            "static uint32_t _wbic_wd_{0};\n"
            // 组合阶段回读主设备应答时要知道本拍选中的从设备和换主空拍
            "static int _wbic_si_{0};\n"
            "static uint8_t _wbic_gap_{0};\n"
            // 上一拍真正驱动过的那一根从设备侧总线 / 主设备侧应答（-1 = 全 0）。
            // 只有被选中的从设备（主设备）有输出，换选时把旧的清回 0
            "static int _wbic_wsi_{0}, _wbic_mlast_{0};\n"
            // 当前持有者的 stb 线指针：授权时算好，组合阶段每拍只解一次引用
            "static uint8_t *_wbic_msp_{0};\n"
            "extern void dcs_wb_trace(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n",
            _id);
}

std::string WishboneInterconnect::genInitCode() const {
    // 轮询从 0 开始；grant/last 为 -1 表示还没有事务，首个事务不需要撤拍
    return std::format(
            "    _wbic_grant_{0} = -1; _wbic_next_{0} = 0; _wbic_last_{0} = -1; _wbic_busy_{0} = 0; _wbic_wd_{0} = 0;\n"
            "    _wbic_si_{0} = 0; _wbic_gap_{0} = 0; _wbic_wsi_{0} = -1; _wbic_mlast_{0} = -1; _wbic_msp_{0} = 0;\n",
            _id);
}

std::string WishboneInterconnect::genFuncDef_seq() const {
    const int M = _masters;
    const int N = numSlaves();
    std::string c;
    c += std::format("// WishboneInterconnect: {} slaves, {} masters\n", N, M);
    c += std::format("static void {}() {{\n", funcName_seq());

    // 从设备侧总线的输出线按选中下标直接寻址。每拍重写 6×N 根线曾是本元件的
    // 全部开销所在（perf 实测互连吃掉整机四成），现在只驱动被选中的那一个，
    // 换选时把上一根的 6 根线清零即可
    c += std::format("    static uint8_t _wbic_sink_{0}[16];\n", _id);
    c += std::format("    static uint8_t *const _wbic_sn_{}[{}][6] = {{\n", _id, N);
    for (int i = 0; i < N; i++) {
        c += "        {";
        for (int k = 0; k < 6; k++) {
            const int nid = outputs()[M * 2 + i * 6 + k]->netId();
            c += std::format("{}{}", k ? ", " : "",
                             nid >= 0 ? std::format("_w[{}]", nid) : "_wbic_sink_" + std::to_string(_id));
        }
        c += std::format("}}{}\n", i + 1 < N ? "," : "");
    }
    c += "    };\n";
    // 主设备 stb 线指针表：授权时取一次存进状态，组合阶段就不用每拍查表再解引用
    c += std::format("    static uint8_t *const _wbic_mstab_{}[{}] = {{", _id, M);
    for (int mi = 0; mi < M; mi++) {
        const int nid = inputs()[mi * 6 + 4]->netId();
        c += std::format("{}{}", mi ? ", " : "",
                         nid >= 0 ? std::format("_w[{}]", nid) : "_wbic_sink_" + std::to_string(_id));
    }
    c += "};\n";

    // 三段式：持锁 → 只读持有者两根线；空闲 → 扫描全部请求线；拿不到就返回
    auto rd = [&](const std::string &var, int pin_idx, int pin_w, const char *ind) {
        const int nid = inputs()[pin_idx]->netId();
        if (nid < 0) {
            c += std::format("{}{} = 0;\n", ind, var);
            return;
        }
        c += std::format("{}dcs_memcpy(&{}, _w[{}], {});\n", ind, var, nid, byte_count(pin_w));
        if (pin_w < 64)
            c += std::format("{}{} &= {};\n", ind, var, gen_mask(pin_w));
    };
    c += "    int _grant = -1;\n";
    c += std::format("    const uint8_t _wbic_was_busy = _wbic_busy_{};\n", _id);
    c += "    uint32_t _adr = 0, _dat = 0; uint8_t _we = 0, _sel = 0, _stb = 0, _cyc = 0;\n";
    // 持锁中只关心持有者还举不举 stb：其它主设备这一拍不可能拿到总线，
    // 它们的请求线等撤锁那一拍再读
    c += std::format("    if (_wbic_was_busy) {{\n"
                     "        switch (_wbic_grant_{}) {{\n",
                     _id);
    for (int mi = 0; mi < M; mi++) {
        c += std::format("        case {}:\n", mi);
        rd("_stb", mi * 6 + 4, 1, "            ");
        rd("_cyc", mi * 6 + 5, 1, "            ");
        c += "            break;\n";
    }
    c += "        default: break;\n        }\n";
    c += std::format("        if (_stb & _cyc) _grant = _wbic_grant_{};\n", _id);
    // 撤锁时把 grant 也清成 -1：组合阶段靠它一眼判断"现在没有主设备在等应答"，
    // 否则空闲拍还要去追一次主设备的 stb 指针才能知道没人等
    c += std::format("        else {{ _wbic_busy_{0} = 0; _wbic_grant_{0} = -1; }}\n", _id);
    c += "    }\n";

    // 空闲/撤锁：这时才把所有主设备的请求线读一遍。
    // 绝大多数拍没有任何主设备在请求，所以先每口只读 stb 一根窄线拼成请求位图，
    // 谁的 stb 拉起来了才去读它那根 cyc 确认（Wishbone 规定没有 stb 就不算事务）。
    // 空闲拍因此只花 M 次字节读，而不是原来的 2M 次
    c += "    uint8_t _gap = 0;\n";
    c += "    if (_grant < 0) {\n";
    c += "    uint32_t _rq = 0;\n";
    for (int mi = 0; mi < M; mi++) {
        const int stb_nid = inputs()[mi * 6 + 4]->netId();
        const int cyc_nid = inputs()[mi * 6 + 5]->netId();
        if (stb_nid < 0)
            continue; // 悬空的主设备口永远不请求
        c += std::format("    const uint8_t _s{0} = _w[{1}][0] & 1;\n", mi, stb_nid);
        c += std::format("    _rq |= (uint32_t) _s{0} << {0};\n", mi);
        if (cyc_nid >= 0)
            c += std::format("    if (_s{0} && !(_w[{1}][0] & 1)) _rq &= ~(1u << {0});\n", mi, cyc_nid);
    }
    // 没有请求、上一拍也没有持锁时，互连不可能产生任何新动作
    c += "    if (!_rq && !_wbic_was_busy) return;\n";

    // 多主设备仲裁：拿到总线的就锁住，直到它撤销 stb 才放行；空闲时从上次授权的
    // 下一个主设备开始扫描。不能用固定优先——CPU 每个周期都有访存请求，固定优先会
    // 让设备的主端口永远排不上队，等主端口回读的请求就永远完不成。
    //
    // 请求汇总成一个位掩码，扫描只做位测试。TCC 不做常量折叠和分支合并，
    // 让每个候选各带一整套"比较 + 搬六个信号"会把生成代码的分支数和体积都翻 M 倍。
    c += std::format("        int _g = _wbic_next_{};\n", _id);
    c += std::format("        if (_g < 0 || _g >= {}) _g = 0;\n", M);
    for (int k = 0; k < M; k++) {
        c += std::format(
                "        if (_grant < 0) {{ int _i = _g + {0}; if (_i >= {1}) _i -= {1}; "
                "if (_rq & (1u << _i)) _grant = _i; }}\n",
                k, M);
    }
    c += "        if (_grant >= 0) {\n";
    // 换主时从设备看到的 stb 可能还是上一拍那一根 1，不撤一拍它就不会产生新的上升沿
    c += std::format("            _gap = (_wbic_last_{0} >= 0 && _wbic_last_{0} != _grant) ? 1 : 0;\n", _id);
    c += std::format("            _wbic_grant_{0} = _grant; _wbic_last_{0} = _grant; _wbic_busy_{0} = 1;\n", _id);
    c += std::format("            _wbic_msp_{0} = _wbic_mstab_{0}[_grant];\n", _id);
    c += std::format("            _wbic_next_{0} = (_grant + 1 == {1}) ? 0 : _grant + 1;\n", _id, M);
    c += "        }\n";
    c += "    }\n";

    // 只搬拿到总线的那一个主设备的信号（新授权和保留授权都走这里）：其余主设备
    // 这一拍没有使用权，它们的地址/数据线读进来也用不上
    c += "    if (_grant >= 0) {\n";
    c += "        switch (_grant) {\n";
    for (int mi = 0; mi < M; mi++) {
        c += std::format("        case {}:\n", mi);
        rd("_adr", mi * 6 + 0, 32, "            ");
        rd("_dat", mi * 6 + 1, 32, "            ");
        rd("_we", mi * 6 + 2, 1, "            ");
        rd("_sel", mi * 6 + 3, 4, "            ");
        rd("_stb", mi * 6 + 4, 1, "            ");
        rd("_cyc", mi * 6 + 5, 1, "            ");
        c += "            break;\n";
    }
    c += "        }\n";
    c += "    }\n";

    // 地址解码：段信息编译期已知，直接生成常量比较链。按段序号排成 else-if，
    // 第一个段（RAM，绝大多数访存都落在这里）只比一次，不用把后面 7 段全比一遍
    c += "    int _si;\n";
    for (int i = 0; i < N; i++) {
        const uint64_t lo = _segments[i].base;
        const uint64_t hi = (uint64_t) _segments[i].base + _segments[i].size;
        c += std::format("    {}if (_adr >= 0x{:X}ULL && _adr < 0x{:X}ULL) _si = {};\n", i ? "else " : "", lo, hi,
                         i);
    }
    c += "    else _si = 0;\n"; // 没有段命中时落到第一个从设备，与原先的默认行为一致
    // 组合阶段回读应答时用这份状态
    c += std::format("    _wbic_si_{} = _si; _wbic_gap_{} = _gap;\n", _id, _id);

    // 从设备侧输出：本拍拿到的总线只送给被选中的从设备，其余从设备保持 0
    c += "    const uint8_t _ss = (uint8_t)(_stb && !_gap), _sc = (uint8_t)(_cyc && !_gap);\n";
    c += "    if (_grant >= 0) {\n";
    c += std::format("        uint8_t *const *_sn = _wbic_sn_{}[_si];\n", _id);
    for (int k = 0; k < 6; k++) {
        static const char *vals[6] = {"_adr", "_dat", "_we", "_sel", "_ss", "_sc"};
        const int w = outputs()[M * 2 + k]->bit_width();
        // 按引脚宽度写，别改成 8 字节整写：下游按字节读同一根线，宽存储等不到存储转发。
        // 超出引脚宽度的高字节从 init 起一直是 0，且这些线只由互连驱动，不用每拍再清
        const int b = byte_count(w);
        c += std::format("        dcs_memcpy(_sn[{}], &{}, {});\n", k, vals[k], b);
    }
    // 换到别的从设备：上一根必须清零，否则它会一直看到上次的 stb。
    // 每条线只清自己写过的字节数——从设备按引脚宽度读线网，多清没用
    auto clear_body = [&](const char *ind) {
        std::string s;
        for (int k = 0; k < 6; k++)
            s += std::format("{}dcs_memset(_old[{}], 0, {});\n", ind, k,
                             byte_count(outputs()[M * 2 + k]->bit_width()));
        return s;
    };
    c += std::format("        if (_wbic_wsi_{0} != _si) {{\n"
                     "            if (_wbic_wsi_{0} >= 0) {{\n"
                     "                uint8_t *const *_old = _wbic_sn_{0}[_wbic_wsi_{0}];\n",
                     _id);
    c += clear_body("                ");
    c += std::format("            }}\n            _wbic_wsi_{} = _si;\n        }}\n", _id);
    c += std::format("    }} else if (_wbic_wsi_{0} >= 0) {{\n"
                     "        uint8_t *const *_old = _wbic_sn_{0}[_wbic_wsi_{0}];\n",
                     _id);
    c += clear_body("        ");
    c += std::format("        _wbic_wsi_{} = -1;\n    }}\n", _id);
    // 主设备拿了总线却一直不撤销：说明被选中的从设备没回 ack，把地址和持有者打出来
    c += std::format(
            "    if (_wbic_busy_{0}) {{\n"
            "        if (_wbic_wd_{0} != 0xFFFFFFFFu) _wbic_wd_{0}++;\n"
            "        if (_wbic_wd_{0} == 20000u)\n"
            "            dcs_wb_trace(_adr, (uint32_t) _grant, (uint32_t) _si, (uint32_t) _stb, (uint32_t) _cyc, 1u);\n"
            "    }} else _wbic_wd_{0} = 0;\n",
            _id);
    c += "}\n";
    return c;
}

std::string WishboneInterconnect::genFuncDef_comb() const {
    const int M = _masters;
    const int N = numSlaves();
    std::string c;
    c += std::format("// WishboneInterconnect comb: {} 个从设备的应答多路选择\n", N);
    c += std::format("static void {}() {{\n", funcName_comb());
    // 主设备侧的 stb/dat/ack 线按持有者下标直接寻址，理由同 seq：
    // 应答只会送给当前持锁的那一个主设备
    c += std::format("    static uint8_t _wbic_sink_{0}[16];\n", _id);
    c += std::format("    static uint8_t *const _wbic_mn_{}[{}][3] = {{\n", _id, M);
    for (int mi = 0; mi < M; mi++) {
        const int stb_nid = inputs()[mi * 6 + 4]->netId();
        const int dat_nid = outputs()[mi * 2]->netId();
        const int ack_nid = outputs()[mi * 2 + 1]->netId();
        auto addr = [&](int nid) {
            return nid >= 0 ? std::format("_w[{}]", nid) : "_wbic_sink_" + std::to_string(_id);
        };
        c += std::format("        {{{}, {}, {}}}{}\n", addr(stb_nid), addr(dat_nid), addr(ack_nid),
                         mi + 1 < M ? "," : "");
    }
    c += "    };\n";
    // 主设备这一拍还在等应答（stb 保持）才去读从设备的应答和读数据；空闲拍只碰
    // 两个状态字就退出去。stb 一撤立刻把 ack 归零，免得下一笔事务把上一笔的
    // 残留 ack 当成自己的
    c += std::format(
            "    const int _g = _wbic_grant_{};\n"
            "    if (_g >= 0 && !_wbic_gap_{} && (_wbic_msp_{}[0] & 1)) {{\n"
            "        uint8_t *const *_mo = _wbic_mn_{}[_g];\n"
            "        uint32_t _sd = 0; uint8_t _sa = 0;\n",
            _id, _id, _id, _id);
    c += std::format("        switch (_wbic_si_{}) {{\n", _id);
    for (int i = 0; i < N; i++) {
        const int dat_nid = inputs()[M * 6 + i * 2]->netId();
        const int ack_nid = inputs()[M * 6 + i * 2 + 1]->netId();
        c += std::format("        case {}:\n", i);
        if (dat_nid >= 0)
            c += std::format("            dcs_memcpy(&_sd, _w[{}], 4);\n", dat_nid);
        if (ack_nid >= 0)
            c += std::format("            _sa = _w[{}][0] & 1;\n", ack_nid);
        c += "            break;\n";
    }
    c += "        default: break;\n";
    c += "        }\n";
    c += std::format(
            // 主设备侧应答线同样只写有效字节；高字节 init 后为 0，无第二驱动源
            "        dcs_memcpy(_mo[1], &_sd, 4);\n"
            "        dcs_memcpy(_mo[2], &_sa, 1);\n"
            "        if (_wbic_mlast_{} != _g) {{\n"
            "            if (_wbic_mlast_{} >= 0) {{\n"
            "                uint8_t *const *_old = _wbic_mn_{}[_wbic_mlast_{}];\n"
            "                dcs_memset(_old[1], 0, 4);\n"
            "                dcs_memset(_old[2], 0, 1);\n"
            "            }}\n"
            "            _wbic_mlast_{} = _g;\n"
            "        }}\n"
            "    }} else if (_wbic_mlast_{} >= 0) {{\n"
            "        uint8_t *const *_old = _wbic_mn_{}[_wbic_mlast_{}];\n"
            "        dcs_memset(_old[1], 0, 4);\n"
            "        dcs_memset(_old[2], 0, 1);\n"
            "        _wbic_mlast_{} = -1;\n"
            "    }}\n",
            _id, _id, _id, _id, _id, _id, _id, _id, _id, _id, _id);
    c += "}\n";
    return c;
}


std::string WishboneInterconnect::checkTopology(const Circuit &circuit) const {
    // 段表里留了一段地址、后面却没接设备：主设备访问到这段会一直等 ack，
    // 表现就是整机在原地空转，跑到一半才发现。接错线不算电路非法，只告警
    std::set<int> consumed;
    for (const auto &c: circuit.components()) {
        if (c.get() == this)
            continue;
        for (const auto &p: c->inputs())
            if (p->net())
                consumed.insert(p->net()->id());
    }
    std::string msg;
    for (int i = 0; i < numSlaves(); i++) {
        // 从设备侧输出的顺序：adr, dat, we, sel, stb, cyc
        const Net *stb = outputs()[(size_t) (_masters * 2 + i * 6 + 4)]->net();
        if (!stb || consumed.contains(stb->id()))
            continue;
        if (!msg.empty())
            msg += "；";
        msg += std::format("从设备端口 s{}（0x{:X} 起 0x{:X} 字节）没有接到任何设备，访问这段地址会永远等不到 ack", i,
                           _segments[i].base, _segments[i].size);
    }
    return msg;
}

std::unique_ptr<Component> WishboneInterconnect::clone(const std::string &new_name) const {
    // 重建 segments 字符串
    std::ostringstream oss;
    for (size_t i = 0; i < _segments.size(); i++) {
        if (i > 0)
            oss << ",";
        oss << _segments[i].base << ":" << _segments[i].size;
    }
    return std::make_unique<WishboneInterconnect>(new_name, oss.str(), _masters);
}

} // namespace dsc

// JIT 的互连卡死检测回调：一个主设备持锁太久，把卡住的地址和持有者打出来
extern "C" void dcs_wb_trace(uint32_t adr, uint32_t grant, uint32_t si, uint32_t stb, uint32_t cyc, uint32_t busy) {
    std::fprintf(stderr, "[dcs] wishbone 卡住 adr=0x%X master=%d slave=%u stb=%u cyc=%u busy=%u\n", adr, (int) grant, si,
                 stb, cyc, busy);
    std::fflush(stderr);
}

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"wishbone_ic", {{"segments", "0:65536"}, {"num_masters", "1"}}, {"m0_adr_i", "m0_dat_i", "m0_we_i", "m0_sel_i", "m0_stb_i", "m0_cyc_i", "s0_dat_i", "s0_ack_i"}, {"m0_dat_o", "m0_ack_o", "s0_adr_o", "s0_dat_o", "s0_we_o", "s0_sel_o", "s0_stb_o", "s0_cyc_o"}, dsc::ComponentCategory::Bus, "Wishbone 地址解码器/MMIO路由器，按地址段分发到多个从设备"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::WishboneInterconnect>(n, dsc::paramStr(p, "segments", "0:65536"), dsc::paramInt(p, "num_masters", 1));
        });
} // namespace
