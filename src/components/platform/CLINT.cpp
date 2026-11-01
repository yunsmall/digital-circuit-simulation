// CLINT — RISC-V 核心局部中断器
// 全部逻辑在 JIT 里展开：每拍只有一次 mtime 递增和几个寄存器比较，
// 没有 C++ 回调、没有打包解包。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/CLINT.h"
#include <cstring>
#include <format>

namespace dsc {

// Wishbone 引脚索引
constexpr int SLOT = 16;
constexpr int I_ADR = 0, I_WDAT = 1, I_WE = 2, I_SEL = 3, I_STB = 4, I_CYC = 5, I_CLK = 6;
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
    addOutput("mti", 1, false, true);
    addOutput("msi", 1, false, true);
}

bool CLINT::prepare(std::string &) { _prepared = true; return true; }

std::vector<Component::JitSymbol> CLINT::extraJitSymbols() const {
    return {}; // 逻辑全部在生成代码里展开
}

std::string CLINT::genStateDecl() const {
    // 寄存器偏移用宏，生成代码里读起来比裸 0xBFF8 清楚；宏名嵌实例 id，
    // 多个 CLINT（或以后的其它元件）各自一份定义，不会互相覆盖
    return std::format(
            "#define _CLINT_{0}_MSIP 0x0000u\n"
            "#define _CLINT_{0}_MTIMECMP 0x4000u\n"
            "#define _CLINT_{0}_MTIME 0xBFF8u\n"
            "static uint64_t _clint_mtime_{0}, _clint_mtimecmp_{0};\n"
            "static uint32_t _clint_msip_{0};\n"
            "static uint8_t _clint_prev_clk_{0}, _clint_prev_stb_{0};\n",
            _id);
}

std::string CLINT::genInitCode() const {
    // mtimecmp 复位成"永不比较"：真实平台靠 M 态固件初始化它，本核没有固件，
    // 留 0 会让 mtime >= mtimecmp 从第一拍就恒真，内核开定时器中断即中断风暴
    return std::format(
            "    _clint_mtime_{0} = 0; _clint_mtimecmp_{0} = ~0ULL; _clint_msip_{0} = 0;\n"
            "    _clint_prev_clk_{0} = 0; _clint_prev_stb_{0} = 0;\n",
            _id);
}

std::string CLINT::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// CLINT {}\n", name());
    c += std::format("static void {}() {{\n", funcName_seq());
    // 空闲快速路径：mtime 只在时钟上升沿推进，其余寄存器只受总线写影响。
    // 两者都没有时本拍状态和输出都不会变，读 clk/stb/cyc 三根线即可返回
    c += "    uint8_t _clk = _w[" + nid(I_CLK) + "][0] & 1;\n";
    c += "    uint8_t _stb = _w[" + nid(I_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(I_CYC) + "][0] & 1;\n";
    c += std::format(
            "    const uint8_t _rising = (!_clint_prev_clk_{0} && _clk) ? 1 : 0;\n"
            "    _clint_prev_clk_{0} = _clk;\n"
            "    const bool _active = _stb && _cyc;\n"
            "    const bool _prev_act = _clint_prev_stb_{0} != 0;\n"
            "    _clint_prev_stb_{0} = _active ? 1 : 0;\n"
            "    if (!_rising && !_active && !_prev_act) return;\n"
            "    if (_rising) _clint_mtime_{0}++;\n",
            _id);
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(I_ADR) + "], 4);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_WDAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _sel = _w[" + nid(I_SEL) + "][0];\n";
    c += std::format(
            "    const bool _rise = _active && !_prev_act;\n"
            "    uint32_t _dout = 0; uint8_t _ack = 0;\n"
            "    if (_active && !_we) {{\n"
            "        switch (_adr & 0xFFFC) {{\n"
            "        case _CLINT_{0}_MSIP: _dout = _clint_msip_{0}; break;\n"
            "        case _CLINT_{0}_MTIMECMP: _dout = (uint32_t)_clint_mtimecmp_{0}; break;\n"
            "        case _CLINT_{0}_MTIMECMP + 4: _dout = (uint32_t)(_clint_mtimecmp_{0} >> 32); break;\n"
            "        case _CLINT_{0}_MTIME: _dout = (uint32_t)_clint_mtime_{0}; break;\n"
            "        case _CLINT_{0}_MTIME + 4: _dout = (uint32_t)(_clint_mtime_{0} >> 32); break;\n"
            "        default: break;\n"
            "        }}\n"
            "        _ack = 1;\n"
            "    }}\n"
            "    if (_rise && _we) {{\n"
            "        switch (_adr & 0xFFFC) {{\n"
            "        case _CLINT_{0}_MSIP:\n",
            _id);
    // msip 按字节使能写入
    for (int b = 0; b < 4; b++) {
        const uint32_t clear_mask = ~(0xFFu << (8 * b));
        c += std::format("            if (_sel & {}) _clint_msip_{} = (_clint_msip_{} & 0x{:08X}u) | (_wdat & 0x{:08X}u);\n",
                         1 << b, _id, _id, clear_mask, (0xFFu << (8 * b)));
    }
    c += "            break;\n";
    // mtimecmp 低半
    c += std::format(
            "        case _CLINT_{0}_MTIMECMP: {{\n"
            "            uint64_t _lo = _clint_mtimecmp_{0} & 0xFFFFFFFFULL;\n", _id);
    for (int b = 0; b < 4; b++) {
        const uint64_t mask = ~(0xFFULL << (8 * b));
        c += std::format("            if (_sel & {}) _lo = (_lo & 0x{:016X}ULL) | ((_wdat & 0x{:08X}u) << {});\n",
                         1 << b, mask, (0xFFu << (8 * b)), 8 * b);
    }
    c += std::format(
            "            _clint_mtimecmp_{0} = (_clint_mtimecmp_{0} & 0xFFFFFFFF00000000ULL) | (_lo & 0xFFFFFFFFULL);\n"
            "            break;\n"
            "        }}\n"
            "        case _CLINT_{0}_MTIMECMP + 4: {{\n"
            "            uint64_t _hi = _clint_mtimecmp_{0} & 0xFFFFFFFF00000000ULL;\n", _id);
    for (int b = 0; b < 4; b++) {
        const uint64_t mask = ~(0xFFULL << (32 + 8 * b));
        c += std::format("            if (_sel & {}) _hi = (_hi & 0x{:016X}ULL) | ((uint64_t)(_wdat & 0x{:08X}u) << {});\n",
                         1 << b, mask, (0xFFu << (8 * b)), 32 + 8 * b);
    }
    c += std::format(
            "            _clint_mtimecmp_{0} = _hi | (_clint_mtimecmp_{0} & 0xFFFFFFFFULL);\n"
            "            break;\n"
            "        }}\n"
            "        default: break;\n"
            "        }}\n"
            "        _ack = 1;\n"
            "    }}\n"
            "    uint8_t _mti = (_clint_mtime_{0} >= _clint_mtimecmp_{0}) ? 1 : 0;\n"
            "    uint8_t _msi = (_clint_msip_{0} & 1u) ? 1 : 0;\n",
            _id);
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_MTI, "_mti", 1) + "\n";
    c += "    " + genOutputWrite(O_MSI, "_msi", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> CLINT::clone(const std::string &n) const {
    return std::make_unique<CLINT>(n);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"clint", {}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "clk"}, {"wb_dat_o", "wb_ack_o", "mti", "msi"}, dsc::ComponentCategory::CPU, "RISC-V CLINT 定时器/软件中断发生器"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> & /*p*/) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::CLINT>(n);
        });
} // namespace
