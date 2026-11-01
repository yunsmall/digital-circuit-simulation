// WishboneInterconnect — Wishbone 地址解码器
// 路由逻辑全部展开进 JIT 生成的 C 代码：没有跨模块调用、没有结构体指针追逐，
// 地址段在编译期折叠成常量比较。这样 LLVM 后端能优化到接近手写，
// TinyCC 直译出来的也只是一串标量比较。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/WishboneInterconnect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
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
            // 上一拍是否写过非零的主设备侧输出：空闲拍靠它跳过重写
            "static uint8_t _wbic_any_{0};\n"
            "extern void dcs_wb_trace(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n",
            _id);
}

std::string WishboneInterconnect::genInitCode() const {
    // 轮询从 0 开始；last = -1 表示还没有事务，首个事务不需要撤拍
    return std::format(
            "    _wbic_grant_{0} = 0; _wbic_next_{0} = 0; _wbic_last_{0} = -1; _wbic_busy_{0} = 0; _wbic_wd_{0} = 0;\n"
            "    _wbic_si_{0} = 0; _wbic_gap_{0} = 0; _wbic_any_{0} = 0;\n",
            _id);
}

std::string WishboneInterconnect::genFuncDef_seq() const {
    const int M = _masters;
    const int N = numSlaves();
    std::string c;
    c += std::format("// WishboneInterconnect: {} slaves, {} masters\n", N, M);
    c += std::format("static void {}() {{\n", funcName_seq());

    // 空闲快速路径：先只读各主设备的 stb/cyc 十根线。没有请求、上一拍也没有
    // 持锁时，互连不可能产生任何新动作（输出在上一拍已经写成全 0），
    // 剩下的 36 根输入和仲裁、看门狗都可以跳过
    for (int mi = 0; mi < M; mi++) {
        c += "    " + gen_read_wire(inputs()[mi * 6 + 4]->netId(), 1, 1, std::format("_m{}_stb", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[mi * 6 + 5]->netId(), 1, 1, std::format("_m{}_cyc", mi)) + "\n";
    }
    c += "    uint32_t _rq = 0;\n";
    for (int mi = 0; mi < M; mi++)
        c += std::format("    _rq |= (uint32_t)(_m{0}_stb & _m{0}_cyc) << {0};\n", mi);
    c += std::format("    const uint8_t _wbic_was_busy = _wbic_busy_{};\n", _id);
    c += "    if (!_rq && !_wbic_was_busy) return;\n";

    // 有请求（或上一拍还持锁）才需要读地址、数据和从设备响应
    for (int mi = 0; mi < M; mi++) {
        int b = mi * 6;
        c += "    " + gen_read_wire(inputs()[b + 0]->netId(), 32, 32, std::format("_m{}_adr", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 1]->netId(), 32, 32, std::format("_m{}_dat", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 2]->netId(), 1, 1, std::format("_m{}_we", mi)) + "\n";
        c += "    " + gen_read_wire(inputs()[b + 3]->netId(), 4, 1, std::format("_m{}_sel", mi)) + "\n";
    }
    for (int i = 0; i < N; i++) {
        c += "    " + gen_read_wire(inputs()[M * 6 + i * 2]->netId(), 32, 32, std::format("_s{}_dat", i)) + "\n";
        c += "    " + gen_read_wire(inputs()[M * 6 + i * 2 + 1]->netId(), 1, 1, std::format("_s{}_ack", i)) + "\n";
    }

    // 多主设备仲裁：拿到总线的就锁住，直到它撤销 stb 才放行；空闲时从上次授权的
    // 下一个主设备开始扫描。不能用固定优先——CPU 每个周期都有访存请求，固定优先会
    // 让设备的主端口永远排不上队，等主端口回读的请求就永远完不成。
    //
    // 请求汇总成一个位掩码，扫描只做位测试。TCC 不做常量折叠和分支合并，
    // 让每个候选各带一整套"比较 + 搬六个信号"会把生成代码的分支数和体积都翻 M 倍。
    c += "    int _grant = -1;\n";
    c += "    uint32_t _adr = 0, _dat = 0; uint8_t _we = 0, _sel = 0, _stb = 0, _cyc = 0;\n";
    c += "    uint8_t _gap = 0;\n";
    // 请求掩码、_wbic_was_busy 和空闲判定已经在函数开头做完
    c += std::format("    if (_wbic_busy_{}) {{\n", _id);
    c += std::format("        const int _bg = _wbic_grant_{};\n", _id);
    c += "        if (_rq & (1u << _bg)) _grant = _bg;\n";
    c += std::format("        else _wbic_busy_{} = 0;\n", _id);
    c += "    }\n";
    c += "    if (_grant < 0) {\n";
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
    c += std::format("            _wbic_next_{0} = (_grant + 1 == {1}) ? 0 : _grant + 1;\n", _id, M);
    c += "        }\n";
    c += "    }\n";
    // 选中后才搬总线信号，M 个 if 里只执行一个，不像扫描那样每个候选都带一整组赋值
    for (int mi = 0; mi < M; mi++) {
        c += std::format(
                "    if (_grant == {0}) {{ _adr = _m{0}_adr; _dat = _m{0}_dat; _we = _m{0}_we; _sel = _m{0}_sel; "
                "_stb = _m{0}_stb; _cyc = _m{0}_cyc; }}\n",
                mi);
    }

    // 地址解码：段信息编译期已知，直接生成常量比较链
    c += "    int _si = 0;\n";
    for (int i = 1; i < N; i++) {
        const uint64_t lo = _segments[i].base;
        const uint64_t hi = (uint64_t) _segments[i].base + _segments[i].size;
        c += std::format("    if (_adr >= 0x{:X}ULL && _adr < 0x{:X}ULL) _si = {};\n", lo, hi, i);
    }
    // 组合阶段回读应答时用这份状态
    c += std::format("    _wbic_si_{} = _si; _wbic_gap_{} = _gap;\n", _id, _id);

    // 主设备侧输出
    c += "    const uint8_t _wbic_out = (uint8_t)(_wbic_was_busy | (_grant >= 0) | (_rq != 0));\n";
    c += "    if (_wbic_out) {\n";
    // 主设备侧应答/读数据不在这里写：它们由组合阶段从被选中从设备的应答直接多路选择
    // 过来（见 genFuncDef_comb），比放进时序阶段少一拍

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
    c += "    }\n";
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
    // 只读被选中从设备的应答和读数据：生成期展开成 switch，不做 2N 次无条件读
    c += "    uint32_t _sd = 0; uint8_t _sa = 0;\n";
    c += std::format("    switch (_wbic_si_{}) {{\n", _id);
    for (int i = 0; i < N; i++) {
        const int dat_nid = inputs()[M * 6 + i * 2]->netId();
        const int ack_nid = inputs()[M * 6 + i * 2 + 1]->netId();
        c += std::format("    case {}:\n", i);
        if (dat_nid >= 0)
            c += std::format("        dcs_memcpy(&_sd, _w[{}], 4);\n", dat_nid);
        if (ack_nid >= 0)
            c += std::format("        _sa = _w[{}][0] & 1;\n", ack_nid);
        c += "        break;\n";
    }
    c += "    default: break;\n";
    c += "    }\n";
    // 主设备这一拍还在等应答（stb 保持）才把 ack/dat 给它；stb 一撤立刻归零，
    // 免得下一笔事务把上一笔的残留 ack 当成自己的。
    // 空闲拍不重写这 2M 根线：只有真有主设备在等应答、或者上一拍写过非零值时
    // 才刷新（撤销那一拍必须写 0，否则 ack 会一直挂着）
    c += std::format("    const int _g = _wbic_grant_{};\n    const uint8_t _gap = _wbic_gap_{};\n", _id, _id);
    for (int mi = 0; mi < M; mi++) {
        const int stb_nid = inputs()[mi * 6 + 4]->netId();
        c += std::format("    uint8_t _ms{0} = ", mi);
        if (stb_nid >= 0)
            c += std::format("_w[{}][0] & 1", stb_nid);
        else
            c += "0";
        c += ";\n";
    }
    c += std::format("    const uint8_t _outg = (uint8_t)((_g >= 0) | _wbic_any_{});\n", _id);
    c += "    if (_outg) {\n";
    c += std::format("    _wbic_any_{} = (uint8_t)(_g >= 0);\n", _id);
    for (int mi = 0; mi < M; mi++) {
        c += std::format(
                "    const uint8_t _sel{0} = (uint8_t)(_g == {0} && !_gap && _ms{0});\n"
                "    uint32_t _md{0} = _sel{0} ? _sd : 0; uint8_t _ma{0} = _sel{0} ? _sa : 0;\n",
                mi);
        c += "    " + genOutputWrite(mi * 2, std::format("_md{}", mi), 32) + "\n";
        c += "    " + genOutputWrite(mi * 2 + 1, std::format("_ma{}", mi), 1) + "\n";
    }
    c += "    }\n";
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
