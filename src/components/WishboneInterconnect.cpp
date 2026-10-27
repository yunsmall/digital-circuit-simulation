// WishboneInterconnect — Wishbone 地址解码器（路由逻辑在 C++ 回调中实现）
#include "dcs/components/WishboneInterconnect.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>
#include <stdexcept>

namespace dsc {

WishboneInterconnect::WishboneInterconnect(const std::string &name, const std::string &segments) :
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

    // ---- 主设备侧输入 ----
    addInput("m_adr_i", 32);
    addInput("m_dat_i", 32);
    addInput("m_we_i", 1);
    addInput("m_sel_i", 4);
    addInput("m_stb_i", 1);
    addInput("m_cyc_i", 1);

    // ---- 每个从设备侧输入（ack, dat） ----
    for (int i = 0; i < N; i++) {
        addInput(std::format("s{}_dat_i", i), 32);
        addInput(std::format("s{}_ack_i", i), 1);
    }

    // ---- 主设备侧输出 ----
    addOutput("m_dat_o", 32, false, true);
    addOutput("m_ack_o", 1, false, true);

    // ---- 每个从设备侧输出（adr, dat, we, sel, stb, cyc） ----
    for (int i = 0; i < N; i++) {
        addOutput(std::format("s{}_adr_o", i), 32, false, true);
        addOutput(std::format("s{}_dat_o", i), 32, false, true);
        addOutput(std::format("s{}_we_o", i), 1, false, true);
        addOutput(std::format("s{}_sel_o", i), 4, false, true);
        addOutput(std::format("s{}_stb_o", i), 1, false, true);
        addOutput(std::format("s{}_cyc_o", i), 1, false, true);
    }

    std::memset(&_rstate, 0, sizeof(_rstate));
    _rstate.num_slaves = N;
    _rstate.segments = _segments.data();
    _rstate.seg_count = N;
}

WishboneInterconnect::~WishboneInterconnect() = default;

bool WishboneInterconnect::prepare(std::string & /*error*/) {
    _prepared = true;
    return true;
}

std::vector<Component::JitSymbol> WishboneInterconnect::extraJitSymbols() const {
    return {{"_dcs_wbic_route", reinterpret_cast<void *>(&WishboneInterconnect::routeTick)}};
}

// ============================================================
// C 代码生成
// ============================================================

std::string WishboneInterconnect::genStateDecl() const {
    return std::format("static void *_wbic_state_{0} = (void *)0x{1:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_rstate));
}

std::string WishboneInterconnect::genFuncDef_seq() const {
    int id = _id;
    int N = numSlaves();

    // ---- 输入: 主设备侧 (6) + 每个从设备 (2) = 6 + 2*N ----
    int ni = 6 + 2 * N;
    // ---- 输出: 主设备侧 (2) + 每个从设备 (6) = 2 + 6*N ----
    int no = 2 + 6 * N;

    std::string code;
    code += std::format("// WishboneInterconnect: {} slaves\n", N);
    code += "extern void _dcs_wbic_route(void *, const uint8_t *, uint8_t *);\n\n";
    code += std::format("static void {}() {{\n", funcName_seq());

    // 读取所有输入线网到局部变量
    code += "    // ---- 主设备侧输入 ----\n";
    code += "    " + gen_read_wire(inputs()[0]->netId(), 32, 32, "_m_adr") + "\n";
    code += "    " + gen_read_wire(inputs()[1]->netId(), 32, 32, "_m_dat") + "\n";
    code += "    " + gen_read_wire(inputs()[2]->netId(), 1, 1, "_m_we") + "\n";
    code += "    " + gen_read_wire(inputs()[3]->netId(), 4, 1, "_m_sel") + "\n";
    code += "    " + gen_read_wire(inputs()[4]->netId(), 1, 1, "_m_stb") + "\n";
    code += "    " + gen_read_wire(inputs()[5]->netId(), 1, 1, "_m_cyc") + "\n";

    for (int i = 0; i < N; i++) {
        code += std::format("    // ---- 从设备 {} 侧输入 ----\n", i);
        code += "    " + gen_read_wire(inputs()[6 + i * 2]->netId(), 32, 32,
                                        std::format("_s{}_dat", i)) + "\n";
        code += "    " + gen_read_wire(inputs()[6 + i * 2 + 1]->netId(), 1, 1,
                                        std::format("_s{}_ack", i)) + "\n";
    }

    // 打包到输入缓冲区
    code += std::format("    uint8_t _in[{}][16];\n", ni);
    code += "    dcs_memset(_in, 0, sizeof(_in));\n";
    code += "    dcs_memcpy(_in[0], &_m_adr, 4);\n";
    code += "    dcs_memcpy(_in[1], &_m_dat, 4);\n";
    code += "    dcs_memcpy(_in[2], &_m_we, 1);\n";
    code += "    dcs_memcpy(_in[3], &_m_sel, 1);\n";
    code += "    dcs_memcpy(_in[4], &_m_stb, 1);\n";
    code += "    dcs_memcpy(_in[5], &_m_cyc, 1);\n";
    for (int i = 0; i < N; i++) {
        code += std::format("    dcs_memcpy(_in[{}], &_s{}_dat, 4);\n", 6 + i * 2, i);
        code += std::format("    dcs_memcpy(_in[{}], &_s{}_ack, 1);\n", 6 + i * 2 + 1, i);
    }

    // 调用 C++ 回调
    code += std::format("    uint8_t _out[{}][16];\n", no);
    code += "    dcs_memset(_out, 0, sizeof(_out));\n";
    code += std::format("    _dcs_wbic_route(_wbic_state_{0}, (const uint8_t *)_in, (uint8_t *)_out);\n", id);

    // 解包输出 → 线网
    code += "    {\n";
    code += "        uint32_t _m_dout=0; uint8_t _m_ack=0;\n";
    code += "        dcs_memcpy(&_m_dout, _out[0], 4);\n";
    code += "        dcs_memcpy(&_m_ack, _out[1], 1);\n";
    code += "        " + genOutputWrite(0, "_m_dout", 32) + "\n";
    code += "        " + genOutputWrite(1, "_m_ack", 1) + "\n";
    for (int i = 0; i < N; i++) {
        int base = 2 + i * 6;
        code += std::format("        uint32_t _s{}_adr=0, _s{}_dat=0;\n", i, i);
        code += std::format("        uint8_t _s{}_we=0, _s{}_sel=0, _s{}_stb=0, _s{}_cyc=0;\n", i, i, i, i);
        code += std::format("        dcs_memcpy(&_s{}_adr, _out[{}], 4);\n", i, base);
        code += std::format("        dcs_memcpy(&_s{}_dat, _out[{}], 4);\n", i, base + 1);
        code += std::format("        dcs_memcpy(&_s{}_we, _out[{}], 1);\n", i, base + 2);
        code += std::format("        dcs_memcpy(&_s{}_sel, _out[{}], 1);\n", i, base + 3);
        code += std::format("        dcs_memcpy(&_s{}_stb, _out[{}], 1);\n", i, base + 4);
        code += std::format("        dcs_memcpy(&_s{}_cyc, _out[{}], 1);\n", i, base + 5);
        // 输出引脚索引: 2(主侧) + i*6
        int out_base = 2 + i * 6;
        code += "        " + genOutputWrite(out_base, std::format("_s{}_adr", i), 32) + "\n";
        code += "        " + genOutputWrite(out_base + 1, std::format("_s{}_dat", i), 32) + "\n";
        code += "        " + genOutputWrite(out_base + 2, std::format("_s{}_we", i), 1) + "\n";
        code += "        " + genOutputWrite(out_base + 3, std::format("_s{}_sel", i), 1) + "\n";
        code += "        " + genOutputWrite(out_base + 4, std::format("_s{}_stb", i), 1) + "\n";
        code += "        " + genOutputWrite(out_base + 5, std::format("_s{}_cyc", i), 1) + "\n";
    }
    code += "    }\n";
    code += "}\n";
    return code;
}

std::unique_ptr<Component> WishboneInterconnect::clone(const std::string &new_name) const {
    // 重建 segments 字符串
    std::ostringstream oss;
    for (size_t i = 0; i < _segments.size(); i++) {
        if (i > 0)
            oss << ",";
        oss << _segments[i].base << ":" << _segments[i].size;
    }
    return std::make_unique<WishboneInterconnect>(new_name, oss.str());
}

// ============================================================
// C++ 回调: 路由逻辑
// ============================================================

void WishboneInterconnect::routeTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs) {
    auto &rs = *static_cast<RouteState *>(state_ptr);
    int N = rs.num_slaves;

    // 解包主设备侧输入 (inputs 布局: [0]=adr, [1]=dat, [2]=we, [3]=sel, [4]=stb, [5]=cyc,
    //                        每个从设备 [6+i*2]=dat_i, [6+i*2+1]=ack_i)
    uint32_t m_adr, m_dat;
    bool m_we, m_stb, m_cyc;
    uint8_t m_sel;
    std::memcpy(&m_adr, inputs + 0, 4);
    std::memcpy(&m_dat, inputs + 16, 4);
    m_we = (inputs[32] & 1) != 0;
    m_sel = inputs[48];
    m_stb = (inputs[64] & 1) != 0;
    m_cyc = (inputs[80] & 1) != 0;

    // 解包从设备侧输入
    std::vector<uint32_t> s_dat(N, 0);
    std::vector<bool> s_ack(N, false);
    for (int i = 0; i < N; i++) {
        std::memcpy(&s_dat[i], inputs + (6 + i * 2) * 16, 4);
        s_ack[i] = (inputs[(6 + i * 2 + 1) * 16] & 1) != 0;
    }

    // ---- 地址解码: 找到匹配的从设备 ----
    int selected = 0; // 默认选中第一个
    for (int i = 0; i < N; i++) {
        uint32_t base = rs.segments[i].base;
        uint32_t size = rs.segments[i].size;
        if (m_adr >= base && m_adr < base + size) {
            selected = i;
            break;
        }
    }

    // ---- 路由: 主设备 → 选中从设备; 选中从设备 → 主设备 ----
    // 输出布局:
    //   [0]=m_dat_o, [1]=m_ack_o
    //   [2+i*6..]: sX_adr_o, sX_dat_o, sX_we_o, sX_sel_o, sX_stb_o, sX_cyc_o

    // 主设备侧输出
    if (m_stb && m_cyc) {
        std::memcpy(outputs + 0, &s_dat[selected], 4); // m_dat_o = selected slave's dat
    } else {
        uint32_t zero = 0;
        std::memcpy(outputs + 0, &zero, 4);
    }
    outputs[16] = s_ack[selected] ? 1 : 0; // m_ack_o = selected slave's ack

    // 从设备侧输出: 只有选中的从设备收到总线信号
    for (int i = 0; i < N; i++) {
        int base = (2 + i * 6) * 16;
        if (i == selected) {
            std::memcpy(outputs + base + 0, &m_adr, 4);  // adr
            std::memcpy(outputs + base + 16, &m_dat, 4); // dat
            outputs[base + 32] = m_we ? 1 : 0;           // we
            outputs[base + 48] = m_sel;                  // sel
            outputs[base + 64] = m_stb ? 1 : 0;          // stb
            outputs[base + 80] = m_cyc ? 1 : 0;          // cyc
        }
        // 未选中的从设备: 所有输出保持 0（stb=cyc=0 表示无事务）
    }
}

} // namespace dsc
