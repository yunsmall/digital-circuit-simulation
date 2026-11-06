#include "dcs/Component.h"
#include "dcs/CodeGenNames.h"
#include <cassert>
#include <format>
#include <stdexcept>

namespace dsc {

// ============================================================
// C代码生成工具函数
// ============================================================

const char *c_int_type(int bit_width) {
    if (bit_width <= 8)
        return "uint8_t";
    if (bit_width <= 16)
        return "uint16_t";
    if (bit_width <= 32)
        return "uint32_t";
    if (bit_width <= 64)
        return "uint64_t";
    // 65~128 位：线网槽本来就是 16 字节，宽线网按字节搬运即可
    return "dcs_u128";
}

int byte_count(int bit_width) {
    return (bit_width + 7) / 8;
}

// 只对 1..64 位有意义：65 位以上返回的是 64 位全 1，拿去与 dcs_u128 运算会
// 截掉高半部。宽于 64 位的元件请按字节处理（见 UnaryGate 的 NOT）
std::string gen_mask(int width) {
    if (width <= 0)
        return "0";
    if (width >= 64)
        return "0xFFFFFFFFFFFFFFFFULL";
    return std::format("((1ULL << {}) - 1)", width);
}

std::string gen_read_wire(int net_id, int pin_w, int net_w, std::string_view var) {
    // dcs_u128 是结构体，不能从 0 直接初始化
    const char *init = pin_w <= 64 ? "0" : "{0}";
    if (net_id < 0)
        return std::format("{} {} = {};", c_int_type(pin_w), var, init);
    int b = byte_count(pin_w);
    std::string s = std::format(R"({} {} = {};
    dcs_memcpy(&{}, {}, {});)",
                                c_int_type(pin_w), var, init, var, cgen::wire(net_id), b);
    // 引脚位宽不是 8 的倍数时，最后一个字节里超出 pin_w 的位是线网上的值。
    // >64 位没有整数类型可用，按字节把最后一段的高位清掉；<=64 位直接用掩码
    if (pin_w > 64) {
        if (int rem = pin_w % 8)
            s += std::format("\n    ((uint8_t *)&{})[{}] &= 0x{:02X};", var, b - 1, (1u << rem) - 1u);
    } else if (net_w > pin_w) {
        s += std::format("\n    {} &= {};", var, gen_mask(pin_w));
    }
    return s;
}

std::string gen_write_wire(int comp_id, int out_idx, std::string_view val, int pin_w) {
    int b = byte_count(pin_w);
    const std::string out = cgen::cOut(comp_id, out_idx);
    return std::format("    dcs_memcpy({}, &{}, {});\n    dcs_memset({} + {}, 0, 16 - {});", out, val, b, out,
                       b, b);
}

// ============================================================
// Component
// ============================================================

Component::Component(const std::string &name, const std::string &type_name) : _name(name), _type_name(type_name) {
}

std::vector<int> Component::getCombinationalDeps(int out_idx) const {
    if (out_idx >= 0 && out_idx < (int) _outputs.size()) {
        const Pin *p = _outputs[out_idx].get();
        // 时序输出天然断开组合路径；三态输出在 oe=0 时也是断开的，静态分析
        // 判断不了使能，一律按"可断开"处理。代价是这类环不能编译期拒绝，
        // 由阶段 2b 迭代到不动点兜底（单驱动三态线网也算，见 computeTriAffected）。
        // 只用于环路检测：三态门仍然要按输入算本拍输出，拓扑排序不能用这个
        // 函数来省略三态/时序元件的先后顺序（见 CodeGen 的 topologicalSort）
        if (p->isSequential() || p->isTriState())
            return {};
    }
    std::vector<int> deps;
    for (int i = 0; i < (int) _inputs.size(); i++)
        deps.push_back(i);
    return deps;
}

std::string Component::genFuncDecl_comb() const {
    return std::format("static void {}(void);", funcName_comb());
}

std::string Component::genFuncDecl_seq() const {
    return std::format("static void {}(void);", funcName_seq());
}

std::string Component::funcName_comb() const {
    return std::format("_update_{}_comb", _id);
}

std::string Component::funcName_seq() const {
    return std::format("_update_{}_seq", _id);
}

std::string Component::stateVarName() const {
    return std::format("_s_{}", _id);
}

std::string Component::stateTypeName() const {
    return std::format("_S_{}", _id);
}

Pin *Component::addInput(const std::string &pin_name, int bit_width, PinEdge edge) {
    assert(bit_width >= 1 && bit_width <= 128);
    auto p = std::make_unique<Pin>(pin_name, PinDir::INPUT, bit_width, this, false, false, edge);
    Pin *ptr = p.get();
    _inputs.push_back(std::move(p));
    return ptr;
}

Pin *Component::addOutput(const std::string &pin_name, int bit_width, bool is_tri_state, bool is_sequential) {
    assert(bit_width >= 1 && bit_width <= 128);
    auto p = std::make_unique<Pin>(pin_name, PinDir::OUTPUT, bit_width, this, is_tri_state, is_sequential);
    Pin *ptr = p.get();
    _outputs.push_back(std::move(p));
    return ptr;
}

std::string Component::genGetWireEdge(int in_idx, PinEdge edge) const {
    if (in_idx < 0 || in_idx >= (int) _inputs.size())
        throw std::logic_error(std::format("元件 {} 的 genGetWireEdge 引脚下标越界: {}", name(), in_idx));
    const Pin *pin = _inputs[in_idx].get();
    const PinEdge declared = pin->edge();
    bool ok = false;
    if (edge == PinEdge::Rising)
        ok = declared == PinEdge::Rising || declared == PinEdge::Change;
    else if (edge == PinEdge::Falling)
        ok = declared == PinEdge::Falling || declared == PinEdge::Change;
    else if (edge == PinEdge::Change)
        ok = declared == PinEdge::Change;
    if (!ok)
        throw std::logic_error(
                std::format("元件 {} 的引脚 {} 没有声明所需的边沿依赖（请求 {}，声明 {}）", name(), pin->name(),
                            static_cast<int>(edge), static_cast<int>(declared)));
    if (edge != PinEdge::Change && pin->bit_width() != 1)
        throw std::logic_error(std::format("元件 {} 的引脚 {} 不是 1 位，不能取上升/下降沿", name(), pin->name()));
    // 伪时钟线：clkgen 声明了每 tick 都是某个沿，直接返回真；反向查询是非法的
    const PinEdge pseudo = pin->net() ? pin->net()->pseudoEdge() : PinEdge::None;
    if (pseudo != PinEdge::None) {
        if (pseudo != edge)
            throw std::logic_error(std::format("元件 {} 的引脚 {} 接在每 tick {} 的伪时钟线上，不能取 {}",
                                               name(), pin->name(), static_cast<int>(pseudo),
                                               static_cast<int>(edge)));
        return "(true)";
    }
    const int nid = pin->netId();
    if (nid < 0)
        return "(false)";
    const int mask = edge == PinEdge::Rising ? 1 : (edge == PinEdge::Falling ? 2 : 4);
    return std::format("((_dcs_edge_flags[{}] & {}) != 0)", nid, mask);
}

std::string Component::genInputPtrArray(std::string_view arr) const {
    // 未连接的引脚 netId 为 -1，直接展开会生成 _w[-1]；统一指向一个恒零槽位
    // 用 static const 初始化：_w 和零槽位都是静态存储，地址是编译期常量，
    // 这样每拍不用在栈上重新构造一遍指针数组
    std::string s = "    static const uint8_t _dcs_zero_slot[16] = {0};\n";
    s += std::format("    static const uint8_t *const {}[{}] = {{", arr, _inputs.size());
    for (size_t i = 0; i < _inputs.size(); i++) {
        int nid = _inputs[i]->netId();
        s += i ? "," : "";
        s += nid >= 0 ? cgen::wire(nid) : std::string("_dcs_zero_slot");
    }
    s += "};\n";
    return s;
}

std::string Component::genOutputPtrArray(int count, bool clear) const {
    std::string s = std::format("    static uint8_t _obuf[{}][16];\n", count);
    if (clear)
        s += "    dcs_memset(_obuf, 0, sizeof(_obuf));\n";
    s += std::format("    static uint8_t *const _op[{}] = {{", count);
    for (int i = 0; i < count; i++) s += std::format("{}_obuf[{}]", i ? "," : "", i);
    s += "};\n";
    return s;
}


std::string Component::genOutputWirePtrArray(std::string_view arr) const {
    // 回调直接写线网：省掉 _obuf 中转和随后的一轮拷贝。前提是这些线网只由本元件驱动、
    // 且 _w 在 init 时清零——回调只写有效字节，槽位高位靠"从没被写过"保持 0
    std::string s = "    static uint8_t _dcs_out_dummy[16] = {0};\n";
    s += std::format("    static uint8_t *const {}[{}] = {{", arr, _outputs.size());
    for (size_t i = 0; i < _outputs.size(); i++) {
        int nid = _outputs[i]->netId();
        s += i ? "," : "";
        s += nid >= 0 ? cgen::wire(nid) : std::string("_dcs_out_dummy");
    }
    s += "};\n";
    return s;
}


std::string Component::genOutputWrite(int out_idx, std::string_view val, int pin_w,
                                      std::string_view drive) const {
    bool ts = out_idx >= 0 && out_idx < (int) _outputs.size() && _outputs[out_idx]->isTriState();
    // 三态输出要参与总线决议，必须写 _c_out_ 并给使能；非三态输出只被下游当普通线网读，
    // 直接写 _w 就够了——省掉 _c_out 那一份拷贝和清零，每拍每个输出少两次写
    if (ts) {
        std::string s = gen_write_wire(_id, out_idx, val, pin_w);
        s += std::format("\n    {} = {};", cgen::cOe(_id, out_idx), drive);
        return s;
    }
    int net_id = out_idx < (int) _outputs.size() ? _outputs[out_idx]->netId() : -1;
    if (net_id < 0)
        return "";
    int b = byte_count(pin_w);
    std::string s = std::format("dcs_memcpy({}, &{}, {});", cgen::wire(net_id), val, b);
    // 线网槽的高 8 字节从 init 起一直是 0：<=64 位的驱动只写低 8 字节，
    // >64 位的驱动写满 16 字节且自带零扩展。所以每拍只需要清当前字节窗口里
    // 超出位宽的那几位，不用再把 16-b 字节全清一遍。
    //
    // 试过两种"更省"的写法，实测都更慢，别改回来：
    // 1. 合成一次 8 字节整写：下游按字节读同一根线，宽存储等不到存储转发（慢 13%）
    // 2. 只补零到线网宽度（1 位线网就不用补）：少了补零反而慢约 3%
    if (b < 8)
        s += std::format("\n    dcs_memset({} + {}, 0, {});", cgen::wire(net_id), b, 8 - b);
    return s;
}

} // namespace dsc
