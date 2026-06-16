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
    return "uint64_t";
}

int byte_count(int bit_width) {
    return (bit_width + 7) / 8;
}

std::string gen_mask(int width) {
    if (width <= 0)
        return "0";
    if (width == 64)
        return "0xFFFFFFFFFFFFFFFFULL";
    return std::format("((1ULL << {}) - 1)", width);
}

std::string gen_read_wire(int net_id, int pin_w, int net_w, std::string_view var) {
    if (net_id < 0)
        return std::format("{} {} = 0;", c_int_type(pin_w), var);
    int b = byte_count(pin_w);
    std::string s = std::format(R"({} {};
    dcs_memcpy(&{}, _w[{}], {});)",
                                c_int_type(pin_w), var, var, net_id, b);
    if (net_w > pin_w)
        s += std::format("\n    {} &= {};", var, gen_mask(pin_w));
    return s;
}

std::string gen_write_wire(int net_id, std::string_view val, int pin_w) {
    if (net_id < 0)
        return "// 输出悬空";
    int b = byte_count(pin_w);
    return std::format(R"(dcs_memcpy(_w[{}], &{}, {});
    dcs_memset(_w[{}] + {}, 0, {});)",
                       net_id, val, b, net_id, b, 16 - b);
}

// ============================================================
// Component
// ============================================================

Component::Component(const std::string &name, const std::string &type_name) : _name(name), _type_name(type_name) {
}

std::vector<int> Component::getCombinationalDeps(int) const {
    std::vector<int> deps;
    for (int i = 0; i < (int) _inputs.size(); i++)
        deps.push_back(i);
    return deps;
}

std::string Component::genFuncDecl() const {
    return std::format("static void {}(void);", funcName());
}

std::string Component::funcName() const {
    return std::format("_update_{}", _id);
}

std::string Component::stateVarName() const {
    return std::format("_s_{}", _id);
}

std::string Component::stateTypeName() const {
    return std::format("_S_{}", _id);
}

Pin *Component::addInput(const std::string &pin_name, int bit_width) {
    assert(bit_width >= 1 && bit_width <= 64);
    auto p = std::make_unique<Pin>(pin_name, PinDir::INPUT, bit_width, this);
    Pin *ptr = p.get();
    _inputs.push_back(std::move(p));
    return ptr;
}

Pin *Component::addOutput(const std::string &pin_name, int bit_width) {
    assert(bit_width >= 1 && bit_width <= 64);
    auto p = std::make_unique<Pin>(pin_name, PinDir::OUTPUT, bit_width, this);
    Pin *ptr = p.get();
    _outputs.push_back(std::move(p));
    return ptr;
}

} // namespace dsc
