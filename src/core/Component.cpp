#include "dcs/Component.h"
#include <cassert>
#include <format>

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
    dcs_memcpy(&{}, _w[{}], {});)",
                                c_int_type(pin_w), var, init, var, net_id, b);
    // 宽引脚按字节数拷贝时已经截掉了高位，不需要再掩码
    if (net_w > pin_w && pin_w <= 64)
        s += std::format("\n    {} &= {};", var, gen_mask(pin_w));
    return s;
}

std::string gen_write_wire(int comp_id, int out_idx, std::string_view val, int pin_w) {
    int b = byte_count(pin_w);
    return std::format(R"(    dcs_memcpy(_c_out_{0}_{1}, &{2}, {3});
    dcs_memset(_c_out_{0}_{1} + {3}, 0, 16 - {3});)",
                       comp_id, out_idx, val, b);
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
        // 判断不了使能，一律按"可断开"处理——经过三态总线的环靠阶段 2b 的
        // 迭代上限兜底，这是框架既有的约定
        if (p->isSequential() || p->isTriState())
            return {};
    }
    std::vector<int> deps;
    for (int i = 0; i < (int) _inputs.size(); i++)
        deps.push_back(i);
    return deps;
}

std::string Component::genFuncDecl_comb() const {
    return std::format("DCS_INLINE void {}(void);", funcName_comb());
}

std::string Component::genFuncDecl_seq() const {
    return std::format("DCS_INLINE void {}(void);", funcName_seq());
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

Pin *Component::addInput(const std::string &pin_name, int bit_width) {
    assert(bit_width >= 1 && bit_width <= 128);
    auto p = std::make_unique<Pin>(pin_name, PinDir::INPUT, bit_width, this);
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

std::string Component::genInputPtrArray(std::string_view arr) const {
    // 未连接的引脚 netId 为 -1，直接展开会生成 _w[-1]；统一指向一个恒零槽位
    // 用 static const 初始化：_w 和零槽位都是静态存储，地址是编译期常量，
    // 这样每拍不用在栈上重新构造一遍指针数组
    std::string s = "    static const uint8_t _dcs_zero_slot[16] = {0};\n";
    s += std::format("    static const uint8_t *const {}[{}] = {{", arr, _inputs.size());
    for (size_t i = 0; i < _inputs.size(); i++) {
        int nid = _inputs[i]->netId();
        s += i ? "," : "";
        s += nid >= 0 ? std::format("_w[{}]", nid) : std::string("_dcs_zero_slot");
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
        s += nid >= 0 ? std::format("_w[{}]", nid) : std::string("_dcs_out_dummy");
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
        s += std::format("\n    _c_oe_{}_{} = {};", _id, out_idx, drive);
        return s;
    }
    int net_id = out_idx < (int) _outputs.size() ? _outputs[out_idx]->netId() : -1;
    if (net_id < 0)
        return "";
    int b = byte_count(pin_w);
    std::string s = std::format("dcs_memcpy(_w[{}], &{}, {});", net_id, val, b);
    // 线网槽的高 8 字节从 init 起一直是 0：<=64 位的驱动只写低 8 字节，
    // >64 位的驱动写满 16 字节且自带零扩展。所以每拍只需要清当前字节窗口里
    // 超出位宽的那几位，不用再把 16-b 字节全清一遍
    if (b < 8)
        s += std::format("\n    dcs_memset(_w[{}] + {}, 0, {});", net_id, b, 8 - b);
    return s;
}

} // namespace dsc
