// PLIC — RISC-V 平台级中断控制器（两个 context：0=M 态、1=S 态）
// 全部逻辑展开在 JIT 里：源数量是构造期常量，claim 扫描和中断判定都按常量展开成
// 顺序比较，没有跨模块回调，也没有运行期循环。
//
// 寄存器布局遵循标准 PLIC（每个 context 的 enable 占 0x80 字节、threshold/claim
// 占 0x1000 字节），内核在 S 态用的是 context 1，偏移写错会让使能和 claim 全部无效，
// 表现为外部中断永远送不到内核。
#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/PLIC.h"

#include <format>

namespace dsc {

namespace {
enum InIdx { I_CLK = 0, I_ADR, I_DAT, I_WE, I_SEL, I_STB, I_CYC, I_SRC };
enum OutIdx { O_DAT = 0, O_ACK, O_IRQ };

constexpr uint32_t OFF_PRIORITY = 0x000000;
constexpr uint32_t OFF_PENDING = 0x001000;
constexpr uint32_t OFF_ENABLE = 0x002000;
constexpr uint32_t OFF_THRESHOLD = 0x200000;
constexpr uint32_t CONTEXTS = 2;
constexpr uint32_t ENABLE_STRIDE = 0x80; // 每个 context 的 enable 区大小
constexpr uint32_t CTX_STRIDE = 0x1000;  // 每个 context 的 threshold/claim 区大小
} // namespace

PLIC::PLIC(const std::string &name, int num_sources) : SequentialComponent(name, "plic"), _num_sources(num_sources) {
    // 位图只有 32 位：越界值钳到可用范围（有意容错，setParam 记录生效值）
    if (_num_sources < 1)
        _num_sources = 1;
    if (_num_sources > MAX_SOURCES)
        _num_sources = MAX_SOURCES;
    setParam("num_sources", std::to_string(_num_sources));
    addInput("clk", 1, PinEdge::Rising);
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


std::string PLIC::genStateDecl() const {
    const int n = _num_sources;
    return std::format(
            "#define _PLIC_{0}_PRIO_END {1}u\n"
            "#define _PLIC_{0}_PEND_END {2}u\n"
            "#define _PLIC_{0}_EN_END {3}u\n"
            "#define _PLIC_{0}_THR_END {4}u\n"
            "static uint32_t _plic_prio_{0}[{5}];\n"
            "static uint32_t _plic_pending_{0};\n"
            "static uint32_t _plic_en_{0}[2], _plic_thr_{0}[2], _plic_insvc_{0}[2];\n"
            "static uint32_t _plic_latched_{0};\n"
            "static uint8_t _plic_prev_stb_{0};\n"
            "static uint32_t _plic_last_src_{0};\n",
            _id, 4 * (n + 1), OFF_PENDING + 4 * (n + 1), OFF_ENABLE + CONTEXTS * ENABLE_STRIDE,
            OFF_THRESHOLD + CONTEXTS * CTX_STRIDE, n + 1);
}

std::string PLIC::genInitCode() const {
    return std::format(
            "    dcs_memset(_plic_prio_{0}, 0, sizeof(_plic_prio_{0}));\n"
            // 规范规定 priority=0 表示"never interrupt"，复位值就是 0；
            // 各源的优先级由内核在使能中断时自己写 1
            "    _plic_pending_{0} = 0;\n"
            "    _plic_en_{0}[0] = 0; _plic_en_{0}[1] = 0;\n"
            "    _plic_thr_{0}[0] = 0; _plic_thr_{0}[1] = 0;\n"
            "    _plic_insvc_{0}[0] = 0; _plic_insvc_{0}[1] = 0;\n"
            "    _plic_latched_{0} = 0; _plic_prev_stb_{0} = 0; _plic_last_src_{0} = 0;\n",
            _id, _num_sources + 1);
}

std::string PLIC::genFuncDef_seq() const {
    const int n = _num_sources;
    // 源 31 时 n+1=32，1u<<32 是未定义行为（x86 上移位量被掩成 0，emask 会变成 0，
    // 使能寄存器被写成全 0，所有中断源都失效）
    const uint32_t emask = n >= 31 ? ~1u : (((1u << (n + 1)) - 1) & ~1u); // 有效源位图，源 0 不存在恒为 0
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// PLIC {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    const bool _clk_pos = " + genGetWireEdge(I_CLK, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";
    // 空闲快速路径：中断源位图和总线握手都没变化时，本拍不可能产生新状态或新输出。
    // 只读 3 根线就返回，省掉地址译码和每拍 16 次优先级比较
    c += "    uint8_t _stb = _w[" + nid(I_STB) + "][0] & 1;\n";
    c += "    uint8_t _cyc = _w[" + nid(I_CYC) + "][0] & 1;\n";
    c += "    uint32_t _src = 0; dcs_memcpy(&_src, _w[" + nid(I_SRC) + "], " +
         std::to_string(byte_count(n)) + "); _src &= " + gen_mask(n) + ";\n";
    c += "    const bool _active = _stb && _cyc;\n";
    // 上一拍总线是否选中本设备：撤销那一拍要清 latched 并刷输出
    c += std::format("    const bool _prev_act = _plic_prev_stb_{} != 0;\n", _id);
    c += std::format("    _plic_prev_stb_{} = _active ? 1 : 0;\n", _id);
    c += std::format("    if (!_active && !_prev_act && _src == _plic_last_src_{}) return;\n", _id);
    c += std::format("    _plic_last_src_{} = _src;\n", _id);
    c += "    uint32_t _adr = 0; dcs_memcpy(&_adr, _w[" + nid(I_ADR) + "], 4);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_DAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint32_t _dout = 0; uint8_t _ack = 0;\n";
    // irq_src 的 bit0 对应源 1，而 pending/enable 用 bit(id) 表示源 id，统一左移一位。
    // 已经被某个 context claim 走的源不再重新挂起
    c += std::format(
            "    const uint32_t _arrived = (_src << 1) & ~(_plic_insvc_{0}[0] | _plic_insvc_{0}[1]);\n"
            "    _plic_pending_{0} |= _arrived;\n"
            "    const bool _rise = _active && !_prev_act;\n"
            "    if (_active && !_rise) {{\n"
            "        _dout = _plic_latched_{0};\n" // 事务进行中保持上一次的应答数据
            "        _ack = 1;\n"
            "    }} else if (_rise) {{\n"
            "        const uint32_t _off = _adr & 0x00FFFFFFu;\n"
            "        if (_off < _PLIC_{0}_PRIO_END) {{\n"
            "            const uint32_t _idr = _off >> 2;\n"
            "            if (!_we) _dout = _plic_prio_{0}[_idr];\n"
            "            else if (_idr >= 1) _plic_prio_{0}[_idr] = _wdat & 7u;\n" // PLIC 优先级只有 3 位
            "            _ack = 1;\n"
            "        }} else if (_off >= 0x1000u && _off < _PLIC_{0}_PEND_END) {{\n"
            // pending 同样是 32 位位图：0x1000 是整个源 0-31 的挂起位，不是每源一个寄存器
            "            const uint32_t _pword = (_off - 0x1000u) >> 2;\n"
            "            if (!_we && _pword == 0) _dout = _plic_pending_{0};\n"
            "            _ack = 1;\n"
            "        }} else if (_off >= 0x2000u && _off < _PLIC_{0}_EN_END) {{\n"
            "            const uint32_t _rel = _off - 0x2000u;\n"
            "            const uint32_t _ctx = _rel >> 7;\n" // 每个 context 0x80 字节
            // enable 是 32 位位图寄存器：字索引 * 4 处的位表示源 (word*32 + bit)，
            // 内核一次写整个字（例如写 0x2080=0x2 表示使能源 1），不是每个源一个寄存器
            "            const uint32_t _word = (_rel & 0x7Fu) >> 2;\n"
            "            if (_word == 0) {{\n"
            "                if (!_we) _dout = _plic_en_{0}[_ctx];\n"
            "                else _plic_en_{0}[_ctx] = _wdat & {1}u;\n"
            "            }}\n"
            "            _ack = 1;\n"
            "        }} else if (_off >= 0x200000u && _off < _PLIC_{0}_THR_END) {{\n"
            "            const uint32_t _rel = _off - 0x200000u;\n"
            "            const uint32_t _ctx = _rel >> 12;\n" // 每个 context 0x1000 字节
            "            const uint32_t _sub = _rel & 0xFFFu;\n"
            "            if (_sub == 0) {{\n"
            "                if (!_we) _dout = _plic_thr_{0}[_ctx];\n"
            "                else _plic_thr_{0}[_ctx] = _wdat & 7u;\n"
            "            }} else if (_sub == 4) {{\n",
            _id, emask);
    // claim：取该 context 使能且待处理中优先级最高者，同优先级取源号小的（顺序扫描天然满足）
    c += std::format(
            // 规范：claim 不受 threshold 影响，所以扫描起点是 0 而不是 context 的 threshold
            "                uint32_t _best = 0, _bestp = 0;\n"
            "                const uint32_t _cand = _plic_pending_{0} & _plic_en_{0}[_ctx];\n",
            _id);
    for (int id = 1; id <= n; ++id) {
        c += std::format(
                "                if ((_cand & {0}u) && _plic_prio_{1}[{2}] > _bestp) {{ _bestp = _plic_prio_{1}[{2}]; _best = {2}u; }}\n",
                (1u << id), _id, id);
    }
    c += std::format(
            "                if (!_we) {{\n"
            "                    if (_best) {{ _plic_pending_{0} &= ~(1u << _best); _plic_insvc_{0}[_ctx] |= (1u << _best); }}\n"
            "                    _dout = _best;\n"
            // complete：源号写回后允许它再次触发。规范要求只在"该源当前已使能"时生效，
            // 否则静默忽略；源 0 不存在，同样忽略
            "                }} else if (_wdat <= {1}u && (_wdat != 0) && (_plic_en_{0}[_ctx] & (1u << _wdat))) _plic_insvc_{0}[_ctx] &= ~(1u << _wdat);\n"
            "            }}\n"
            "            _ack = 1;\n"
            "        }} else {{\n"
            "            _ack = 1;\n" // 未实现的偏移按 0 读、写忽略，但不挂死总线
            "        }}\n"
            "    }}\n"
            "    if (_rise) _plic_latched_{0} = _dout;\n"
            "    if (!_active) _plic_latched_{0} = 0;\n",
            _id, n);
    // 中断判定：任一 context 上存在"已使能、待处理、优先级高于阈值"的源就拉高。
    // 输出只有一根线，接到 CPU 的哪个中断引脚由外部连线决定
    c += std::format("    uint8_t _irq = 0;\n");
    for (int ctx = 0; ctx < (int) CONTEXTS; ++ctx) {
        c += std::format("    const uint32_t _pend{0} = _plic_pending_{1} & _plic_en_{1}[{0}];\n", ctx, _id);
        for (int id = 1; id <= n; ++id) {
            c += std::format(
                    "    if ((_pend{0} & {1}u) && _plic_prio_{2}[{3}] > _plic_thr_{2}[{0}]) _irq = 1;\n", ctx,
                    (1u << id), _id, id);
        }
    }
    c += "    " + genOutputWrite(O_DAT, "_dout", 32) + "\n";
    c += "    " + genOutputWrite(O_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_IRQ, "_irq", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> PLIC::clone(const std::string &new_name) const {
    return std::make_unique<PLIC>(new_name, _num_sources);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"plic", {{"num_sources", "8"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "irq_src"}, {"wb_dat_o", "wb_ack_o", "irq"}, dsc::ComponentCategory::CPU, "RISC-V PLIC 平台级中断控制器，单 context"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::PLIC>(n, dsc::paramInt(p, "num_sources", 8));
        });
} // namespace
