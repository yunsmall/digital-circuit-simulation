// WishboneMemory — Wishbone 从设备存储器（访存逻辑在 C++ 回调中实现）
#include "dcs/ComponentFactory.h"
#include "dcs/components/memory/WishboneMemory.h"
#include "dcs/util/MemoryDepth.h"

#include <cstring>
#include <format>
#include <stdexcept>

namespace dsc {

// ============================================================
// 引脚布局常量
// ============================================================
constexpr int SLOT = 16;

// 输入引脚索引（按 addInput 顺序）
constexpr int I_ADR = 0;
constexpr int I_WDAT = 1;
constexpr int I_WE = 2;
constexpr int I_SEL = 3;
constexpr int I_STB = 4;
constexpr int I_CYC = 5;
constexpr int I_CLK = 6;

// 输出引脚索引
constexpr int O_DAT = 0;
constexpr int O_ACK = 1;
constexpr int O_COUNT = 2;

WishboneMemory::WishboneMemory(const std::string &name, int addr_width, int data_width) :
    SequentialComponent(name, "wishbone_mem"),
    _addr_width(addr_width), _data_width(data_width), _depth(depthFromAddrWidth(addr_width)) {

    // 数据引脚与内部暂存都是 32 位，64 位数据会被截断并在移位时产生未定义行为
    if (data_width != 8 && data_width != 16 && data_width != 32)
        throw std::invalid_argument(std::format("数据位宽必须为 8/16/32，给定{}", data_width));

    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));

    // Wishbone 从设备输入（必须与 I_* constexpr 顺序一致）
    addInput("wb_adr_i", 32);  // I_ADR = 0
    addInput("wb_dat_i", 32);  // I_WDAT = 1
    addInput("wb_we_i", 1);    // I_WE = 2
    addInput("wb_sel_i", 4);   // I_SEL = 3
    addInput("wb_stb_i", 1);   // I_STB = 4
    addInput("wb_cyc_i", 1);   // I_CYC = 5
    addInput("clk", 1);        // I_CLK = 6

    // Wishbone 从设备输出（按 O_* 顺序）
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_ack_o", 1, false, true);
}

WishboneMemory::~WishboneMemory() = default;

bool WishboneMemory::prepare(Context &, std::string & /*error*/) {
    allocateMem();
    return true;
}

void WishboneMemory::allocateMem() {
    if (_mem.size() != (size_t) _depth)
        _mem.assign((size_t) _depth, 0);
}

// ============================================================
// C 代码生成：访存协议整体内联进 JIT
// ============================================================

std::string WishboneMemory::genStateDecl() const {
    // 内存缓冲与每拍状态都放在 C 侧：JIT 代码按固定地址直接读写
    return std::format(
            "static uint8_t *const _wbm_mem_{0} = (uint8_t *)0x{1:X}ULL;\n"
            "typedef struct {{ unsigned char prev_stb, prev_we, ack_pending; "
            "unsigned int latched_addr, latched_dout; }} _WBM_S{0};\n"
            "static _WBM_S{0} _wbm_s_{0};\n",
            _id, reinterpret_cast<uintptr_t>(_mem.data()));
}

std::string WishboneMemory::genInitCode() const {
    // 握手状态位必须复位：circuit_reset 后若还留着上一轮的 prev_stb / ack_pending，
    // 首个事务会被误判成"事务进行中"
    return std::format("    dcs_memset(&_wbm_s_{}, 0, sizeof(_wbm_s_{}));\n", _id, _id);
}

std::string WishboneMemory::genFuncDef_seq() const {
    const int id = _id;
    const int nb = byte_count(_data_width);
    const uint32_t mask = (uint32_t) _depth - 1;
    // 端口宽度决定 ADR 的低位语义：32 位端口 ADR(1:0) 恒为 0，16/8 位端口
    // 分别是 ADR(0)/无低位。固定按 4 字节对齐会把 8/16 位访问的地址折错
    const uint32_t align_mask = ~((uint32_t) nb - 1u);

    std::string code;
    code += std::format("// WishboneMemory: {}\n", cgen::commentText(name()));
    code += std::format("static void {}() {{\n", funcName_seq());
    code += "    " + gen_read_wire(inputs()[I_STB]->netId(), 1, 1, "_stb") + "\n";
    code += "    " + gen_read_wire(inputs()[I_CYC]->netId(), 1, 1, "_cyc") + "\n";
    code += std::format(R"(
    // 空闲快速路径：没有在途事务、上一拍也没选中本设备时，存储器和输出都不会变。
    // prev_stb 为 0 也意味着 prev_we 为 0（两者都只在事务有效时置位），可以一起跳过
    const bool _active = _stb && _cyc;
    if (!_active && !_wbm_s_{0}.prev_stb && !_wbm_s_{0}.ack_pending) return;
)", id);
    code += "    " + gen_read_wire(inputs()[I_ADR]->netId(), 32, 32, "_adr") + "\n";
    code += "    " + gen_read_wire(inputs()[I_WDAT]->netId(), 32, 32, "_wdat") + "\n";
    code += "    " + gen_read_wire(inputs()[I_WE]->netId(), 1, 1, "_we") + "\n";
    code += "    " + gen_read_wire(inputs()[I_SEL]->netId(), 4, 4, "_sel") + "\n";
    code += std::format(R"(
    // stb 上升沿：新事务；同一事务内 we 翻转：AMO 的写阶段
    const bool _rise = _active && !_wbm_s_{0}.prev_stb;
    const bool _werise = _active && _we && !_wbm_s_{0}.prev_we;
    _wbm_s_{0}.prev_stb = _active;
    _wbm_s_{0}.prev_we = _active && _we;
    uint32_t _dout = 0;
    uint8_t _ack8 = 0;
    if (_rise) {{
        // Wishbone B4 RULE 3.100：ADR 只到端口字边界，低 log2(字节数) 位不参与
        // 寻址；SEL(i) 对应 DAT(8i+7..8i)，小端时就是该字的第 i 个字节。
        // 从设备只有 addr_width 根地址线，高位在硬件上就是别名回来，按容量折算
        const uint32_t _a = ((_adr & {1}u) & {4}u);
        _wbm_s_{0}.latched_addr = _a;
        if (!_we) {{
            _wbm_s_{0}.latched_dout = 0;
            if ((unsigned long long)_a + {2}ull <= {3}ull)
                dcs_memcpy(&_wbm_s_{0}.latched_dout, _wbm_mem_{0} + _a, {2});
        }} else {{
            if ((unsigned long long)_a + {2}ull <= {3}ull)
                for (int _b = 0; _b < {2}; _b++)
                    if (_sel & (1u << _b))
                        _wbm_mem_{0}[_a + _b] = (uint8_t)((_wdat >> (8 * _b)) & 0xFF);
            _wbm_s_{0}.latched_dout = 0;
        }}
        _wbm_s_{0}.ack_pending = 1;
    }} else if (_werise) {{
        // AMO 写阶段：用已折算过的锁存地址（已经是字对齐的）
        const uint32_t _a = _wbm_s_{0}.latched_addr;
        if ((unsigned long long)_a + {2}ull <= {3}ull)
            for (int _b = 0; _b < {2}; _b++)
                if (_sel & (1u << _b))
                    _wbm_mem_{0}[_a + _b] = (uint8_t)((_wdat >> (8 * _b)) & 0xFF);
    }}
    // 事务有效期间保持 ack；stb 撤销时立刻撤销，避免主设备把残留 ack 当成新结果
    if (_wbm_s_{0}.ack_pending) {{
        if (!_active) _wbm_s_{0}.ack_pending = 0;
        else {{ _dout = _wbm_s_{0}.latched_dout; _ack8 = 1; }}
    }}
)",
                       id, mask, nb, (unsigned long long) _depth, align_mask);
    // 输出直接写线网：这两个线网只由本元件驱动、init 时清零，
    // 只写有效字节即可，不必再补高位清零
    int dat_nid = outputs()[O_DAT]->netId();
    int ack_nid = outputs()[O_ACK]->netId();
    if (dat_nid >= 0)
        code += std::format("    dcs_memcpy(_w[{}], &_dout, 4);\n", dat_nid);
    if (ack_nid >= 0)
        code += std::format("    _w[{}][0] = _ack8;\n", ack_nid);
    code += "}\n";
    return code;
}

std::unique_ptr<Component> WishboneMemory::clone(const std::string &new_name) const {
    auto m = std::make_unique<WishboneMemory>(new_name, _addr_width, _data_width);
    std::memcpy(m->_mem.data(), _mem.data(), _mem.size());
    return m;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"wishbone_mem", {{"addr_width", "16"}, {"data_width", "32", {"8", "16", "32"}}}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "clk"}, {"wb_dat_o", "wb_ack_o"}, dsc::ComponentCategory::Memory, "Wishbone 从设备存储器，支持 IStorage 加载程序"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::WishboneMemory>(n, dsc::paramInt(p, "addr_width", 16), dsc::paramInt(p, "data_width", 32));
        });
} // namespace
