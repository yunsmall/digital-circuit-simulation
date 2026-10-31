// WishboneInterconnect — Wishbone 地址解码器
// 路由逻辑全部展开进 JIT 生成的 C 代码：没有跨模块调用、没有结构体指针追逐，
// 地址段在编译期折叠成常量比较。这样 LLVM 后端能优化到接近手写，
// TinyCC 直译出来的也只是一串标量比较。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/WishboneInterconnect.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>
#include <stdexcept>

namespace dsc {

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

    int N = (int) _segments.size();
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

bool WishboneInterconnect::prepare(std::string & /*error*/) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> WishboneInterconnect::extraJitSymbols() const {
    return {}; // 路由逻辑全部在生成代码里展开，不再有 C++ 回调
}

// ============================================================
// C 代码生成
// ============================================================

std::string WishboneInterconnect::genStateDecl() const {
    return std::format("static int _wbic_grant_{};\n", _id);
}

std::string WishboneInterconnect::genInitCode() const {
    // 轮询仲裁的起点；上一轮遗留的授权会让首个事务被误判成"换主"
    return std::format("    _wbic_grant_{} = 0;\n", _id);
}

std::string WishboneInterconnect::genFuncDef_seq() const {
    const int M = _masters;
    const int N = numSlaves();
    std::string c;
    c += std::format("// WishboneInterconnect: {} slaves, {} masters\n", N, M);
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());

    // 主设备输入全部解包成标量
    for (int mi = 0; mi < M; mi++) {
        int b = mi * 6;
        c += "    " + gen_read_wire(inputs()[b + 0]->netId(), 32, 32, std::format("_m{}_adr", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 1]->netId(), 32, 32, std::format("_m{}_dat", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 2]->netId(), 1, 1, std::format("_m{}_we", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 3]->netId(), 4, 1, std::format("_m{}_sel", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 4]->netId(), 1, 1, std::format("_m{}_stb", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 5]->netId(), 1, 1, std::format("_m{}_cyc", mi)) + "\n";
    }
    for (int i = 0; i < N; i++) {
        c += "    " + gen_read_wire(inputs()[M * 6 + i * 2]->netId(), 32, 32, std::format("_s{}_dat", i)) + "\n";
        c += "    " + gen_read_wire(inputs()[M * 6 + i * 2 + 1]->netId(), 1, 1, std::format("_s{}_ack", i)) + "\n";
    }

    // 事务级轮询：从上次授权的主设备开始扫描。展开成 M 个候选，避免数组和循环
    c += std::format("    int _g = _wbic_grant_{};\n", _id);
    c += std::format("    if (_g < 0 || _g >= {}) _g = 0;\n", M);
    c += "    int _grant = -1;\n";
    c += "    uint32_t _adr = 0, _dat = 0; uint8_t _we = 0, _sel = 0, _stb = 0, _cyc = 0;\n";
    for (int k = 0; k < M; k++) {
        c += std::format("    if (_grant < 0) {{ int _i = _g + {}; if (_i >= {}) _i -= {};\n", k, M, M);
        for (int mi = 0; mi < M; mi++) {
            c += std::format(
                    "        if (_i == {} && _m{}_stb && _m{}_cyc) {{ _grant = {}; _adr = _m{}_adr; _dat = _m{}_dat; "
                    "_we = _m{}_we; _sel = _m{}_sel; _stb = _m{}_stb; _cyc = _m{}_cyc; }}\n",
                    mi, mi, mi, mi, mi, mi, mi, mi, mi, mi);
        }
        c += "    }\n";
    }
    c += std::format("    const int _prev = _wbic_grant_{};\n", _id);
    c += "    const uint8_t _gap = (_prev >= 0 && _grant >= 0 && _grant != _prev) ? 1 : 0;\n";
    c += std::format("    if (_grant >= 0) _wbic_grant_{} = _grant;\n", _id);

    // 地址解码：段信息编译期已知，直接生成常量比较链
    c += "    int _si = 0;\n";
    for (int i = 1; i < N; i++) {
        const uint64_t lo = _segments[i].base;
        const uint64_t hi = (uint64_t) _segments[i].base + _segments[i].size;
        c += std::format("    if (_adr >= 0x{:X}ULL && _adr < 0x{:X}ULL) _si = {};\n", lo, hi, i);
    }

    // 主设备侧输出
    for (int mi = 0; mi < M; mi++) {
        c += std::format("    uint32_t _mo{}_dat = 0; uint8_t _mo{}_ack = 0;\n", mi, mi);
        c += std::format("    if (_grant == {} && !_gap) {{\n", mi);
        for (int i = 0; i < N; i++)
            c += std::format("        if (_si == {}) {{ _mo{}_dat = _s{}_dat; _mo{}_ack = _s{}_ack; }}\n",
                             i, mi, i, mi, i);
        c += "    }\n";
        c += "    " + genOutputWrite(mi * 2, std::format("_mo{}_dat", mi), 32) + "\n";
        c += "    " + genOutputWrite(mi * 2 + 1, std::format("_mo{}_ack", mi), 1) + "\n";
    }

    // 从设备侧输出：只有被选中的从设备收到总线信号
    for (int i = 0; i < N; i++) {
        const int base = M * 2 + i * 6;
        c += std::format(
                "    uint32_t _so{0}_adr = 0, _so{0}_dat = 0; uint8_t _so{0}_we = 0, _so{0}_sel = 0, "
                "_so{0}_stb = 0, _so{0}_cyc = 0;\n",
                i);
        c += std::format(
                "    if (_si == {0} && _grant >= 0) {{ _so{0}_adr = _adr; _so{0}_dat = _dat; _so{0}_we = _we; "
                "_so{0}_sel = _sel; _so{0}_stb = _stb && !_gap; _so{0}_cyc = _cyc && !_gap; }}\n",
                i);
        c += "    " + genOutputWrite(base + 0, std::format("_so{}_adr", i), 32) + "\n";
        c += "    " + genOutputWrite(base + 1, std::format("_so{}_dat", i), 32) + "\n";
        c += "    " + genOutputWrite(base + 2, std::format("_so{}_we", i), 1) + "\n";
        c += "    " + genOutputWrite(base + 3, std::format("_so{}_sel", i), 4) + "\n";
        c += "    " + genOutputWrite(base + 4, std::format("_so{}_stb", i), 1) + "\n";
        c += "    " + genOutputWrite(base + 5, std::format("_so{}_cyc", i), 1) + "\n";
    }
    c += "}\n";
    return c;
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

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"wishbone_ic", {{"segments", "0:65536"}, {"num_masters", "1"}}, {"m0_adr_i", "m0_dat_i", "m0_we_i", "m0_sel_i", "m0_stb_i", "m0_cyc_i", "s0_dat_i", "s0_ack_i"}, {"m0_dat_o", "m0_ack_o", "s0_adr_o", "s0_dat_o", "s0_we_o", "s0_sel_o", "s0_stb_o", "s0_cyc_o"}, dsc::ComponentCategory::Bus, "Wishbone 地址解码器/MMIO路由器，按地址段分发到多个从设备"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::WishboneInterconnect>(n, dsc::paramStr(p, "segments", "0:65536"), dsc::paramInt(p, "num_masters", 1));
        });
} // namespace
