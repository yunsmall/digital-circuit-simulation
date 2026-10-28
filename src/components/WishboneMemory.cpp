// WishboneMemory — Wishbone 从设备存储器（访存逻辑在 C++ 回调中实现）
#include "dcs/components/WishboneMemory.h"

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
constexpr int I_COUNT = 7;

// 输出引脚索引
constexpr int O_DAT = 0;
constexpr int O_ACK = 1;
constexpr int O_COUNT = 2;

WishboneMemory::WishboneMemory(const std::string &name, int addr_width, int data_width) :
    SequentialComponent(name, "wishbone_mem"),
    _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width) {

    if (data_width != 8 && data_width != 16 && data_width != 32 && data_width != 64)
        throw std::invalid_argument(std::format("数据位宽必须为 8/16/32/64，给定{}", data_width));

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

    _mem.resize(_depth, 0);

    std::memset(&_mstate, 0, sizeof(_mstate));
    _mstate.mem = _mem.data();
    _mstate.mem_size = _mem.size();
    _mstate.data_bytes = byte_count(_data_width);
}

WishboneMemory::~WishboneMemory() = default;

bool WishboneMemory::prepare(std::string & /*error*/) {
    _prepared = true;
    return true;
}

// 复位只清握手状态，存储内容由调用方决定是否保留
void WishboneMemory::memReset(void *state_ptr) {
    auto &ms = *static_cast<MemState *>(state_ptr);
    ms.prev_stb = false;
    ms.prev_we = false;
    ms.ack_pending = false;
    ms.latched_dout = 0;
    ms.latched_addr = 0;
}

std::vector<Component::JitSymbol> WishboneMemory::extraJitSymbols() const {
    return {{"_dcs_wbm_tick", reinterpret_cast<void *>(&WishboneMemory::memTick)},
            {"_dcs_wbm_reset", reinterpret_cast<void *>(&WishboneMemory::memReset)}};
}

// ============================================================
// C 代码生成（最小化：只做引脚搬运）
// ============================================================

std::string WishboneMemory::genStateDecl() const {
    return std::format("static void *_wbm_state_{0} = (void *)0x{1:X}ULL;\n", _id,
                       reinterpret_cast<uintptr_t>(&_mstate));
}

std::string WishboneMemory::genInitCode() const {
    // 握手状态位必须复位：circuit_reset 后若还留着上一轮的 prev_stb / ack_pending，
    // 首个事务会被误判成"事务进行中"
    return std::format("    _dcs_wbm_reset(_wbm_state_{});\n", _id);
}

std::string WishboneMemory::genFuncDef_seq() const {
    int id = _id;

    std::string code;
    code += std::format("// WishboneMemory: {}\n", name());
    code += "extern void _dcs_wbm_tick(void *, const uint8_t *const *, uint8_t *const *);\n";
    code += "extern void _dcs_wbm_reset(void *);\n\n";
    code += std::format("static void {}() {{\n", funcName_seq());

    // 读取所有输入线网（按 I_* 顺序）


    code += genInputPtrArray("_in");
    code += std::format("    uint8_t _obuf[{}][{}];\n", O_COUNT, SLOT);
    code += "    dcs_memset(_obuf, 0, sizeof(_obuf));\n";
    code += std::format("    uint8_t *_op[{}] = {{", O_COUNT);
    for (int i = 0; i < O_COUNT; i++) code += std::format("{}_obuf[{}]", i ? "," : "", i);
    code += "};\n";
    code += std::format("    _dcs_wbm_tick(_wbm_state_{0}, _in, _op);\n", id);
    const int ow[O_COUNT] = {32, 1};
    for (int i = 0; i < O_COUNT; i++)
        code += std::format("    {{ uint32_t _v=0; dcs_memcpy(&_v,_obuf[{}],4); {} }}\n", i,
                            genOutputWrite(i, "_v", ow[i]));
    code += "}\n";
    return code;
}

std::unique_ptr<Component> WishboneMemory::clone(const std::string &new_name) const {
    auto m = std::make_unique<WishboneMemory>(new_name, _addr_width, _data_width);
    std::memcpy(m->_mem.data(), _mem.data(), _mem.size());
    return m;
}

// ============================================================
// C++ 回调: 访存逻辑
// ============================================================

void WishboneMemory::memTick(void *state_ptr, const uint8_t *const *inputs, uint8_t *const *outputs) {
    auto &ms = *static_cast<MemState *>(state_ptr);

    // 输入直接就是线网
    uint32_t adr, wdat;
    bool we, stb, cyc;
    uint8_t sel;
    std::memcpy(&adr, inputs[I_ADR], 4);
    std::memcpy(&wdat, inputs[I_WDAT], 4);
    we = (inputs[I_WE][0] & 1) != 0;
    sel = inputs[I_SEL][0];
    stb = (inputs[I_STB][0] & 1) != 0;
    cyc = (inputs[I_CYC][0] & 1) != 0;

    // 默认输出
    uint32_t dout = 0;
    bool ack = false;

    // stb 上升沿检测：新事务到来
    bool stb_active = stb && cyc;
    bool stb_rise = stb_active && !ms.prev_stb;
    // we 上升沿检测：同一事务内 we 翻转（AMO 读后写）
    bool we_rise = stb_active && we && !ms.prev_we;
    ms.prev_stb = stb_active;
    ms.prev_we = stb_active && we;

    if (stb_rise) {
        // 锁存地址（后续 AMO 写阶段可能用到）
        ms.latched_addr = adr;
        if (!we) {
            // 读：锁存结果，保持直到 stb 拉低
            size_t addr = (size_t) adr;
            ms.latched_dout = 0;
            if (addr + ms.data_bytes <= ms.mem_size)
                std::memcpy(&ms.latched_dout, ms.mem + addr, ms.data_bytes);
            ms.ack_pending = true;
        } else {
            // 写：立即完成，锁存 ack
            size_t addr = (size_t) adr;
            // 按数据宽度写，不能硬编码 4 字节：8/16 位配置会越界并写错字节数
            if (addr + ms.data_bytes <= ms.mem_size) {
                for (int b = 0; b < ms.data_bytes; ++b)
                    if (sel & (1u << b))
                        ms.mem[addr + b] = (uint8_t)((wdat >> (8 * b)) & 0xFF);
            }
            ms.latched_dout = 0;
            ms.ack_pending = true;
        }
    } else if (we_rise) {
        // AMO 写阶段：同一事务内 we 从 0 翻 1，用已锁存的地址
        size_t addr = (size_t) ms.latched_addr;
        if (addr + ms.data_bytes <= ms.mem_size) {
            for (int b = 0; b < ms.data_bytes; ++b)
                if (sel & (1u << b))
                    ms.mem[addr + b] = (uint8_t)((wdat >> (8 * b)) & 0xFF);
        }
        // ack 保持高（读阶段已设置），CPU 通过 bus_we 状态区分读写确认
    }

    // 事务有效期间保持 ack；stb 撤销时立刻撤销 ack。
    // 不这样做的话，主设备发起下一次事务的第一拍会看到上一次的残留 ack，
    // 把旧数据当成新事务的结果读取。
    if (ms.ack_pending) {
        if (!stb_active) {
            ms.ack_pending = false;
        } else {
            dout = ms.latched_dout;
            ack = true;
        }
    }

    // 输出直接写线网
    std::memcpy(outputs[O_DAT], &dout, 4);
    outputs[O_ACK][0] = ack ? 1 : 0;
}

} // namespace dsc
