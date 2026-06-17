// PriorityEncoder — 优先编码器实现
#include "dcs/components/PriorityEncoder.h"
#include <cmath>
#include <format>
#include <stdexcept>

namespace dsc {

PriorityEncoder::PriorityEncoder(const std::string &name, int num_inputs) :
    CombinationalComponent(name, "prienc"), _n(num_inputs) {
    setParam("num_inputs", std::to_string(num_inputs));
    if (num_inputs < 2 || num_inputs > 64)
        throw std::invalid_argument("输入数量必须在2-64之间");
    _out_bits = (int) std::ceil(std::log2(num_inputs));

    for (int i = 0; i < num_inputs; i++)
        addInput(std::format("in{}", i), 1);
    addOutput("out", _out_bits);
    addOutput("valid", 1);
}

std::string PriorityEncoder::genFuncDef_comb() const {
    int o_nid = outputs()[0]->netId();
    int v_nid = outputs()[1]->netId();

    // 读取所有输入位
    std::string reads;
    for (int i = 0; i < _n; i++) {
        int nid = inputs()[i]->netId();
        reads += "    " +
                 gen_read_wire(nid, 1, nid >= 0 ? inputs()[i]->net()->bit_width() : 0, std::format("_b{}", i)) + "\n";
    }

    // 从高位到低位查找第一个为1的位
    std::string code;
    code += std::format("static void {}() {{\n", funcName_comb());
    code += reads;
    code += std::format("    {} _out = 0;\n", c_int_type(_out_bits));
    code += "    uint8_t _valid = 0;\n";
    code += std::format("    for (int _i = {}; _i >= 0; _i--) {{\n", _n - 1);
    code += std::format("        if (_b##_i) {{ _out = _i; _valid = 1; break; }}\n");
    code += "    }\n";
    // 上面用了 _b##_i 变名，没法展开……改用switch或链式if

    // 改用链式 if-else 从高位到低位检查
    code = std::format("static void {}() {{\n", funcName_comb());
    code += reads;
    code += std::format("    {} _out = 0;\n", c_int_type(_out_bits));
    code += "    uint8_t _valid = 0;\n";
    for (int i = _n - 1; i >= 0; i--) {
        code += std::format("    {}if (_b{}) {{ _out = {}; _valid = 1; }}\n", i == _n - 1 ? "" : "else ", i, i);
    }

    // 写输出
    if (o_nid >= 0)
        code += std::format("    {}\n    _c_oe_{}_0 = true;\n", genOutputWrite(0, "_out", _out_bits), _id);
    if (v_nid >= 0)
        code += std::format("    {}\n    _c_oe_{}_1 = true;\n", genOutputWrite(1, "_valid", 1), _id);
    code += "}\n";
    return code;
}

std::unique_ptr<Component> PriorityEncoder::clone(const std::string &n) const {
    return std::make_unique<PriorityEncoder>(n, _n);
}

} // namespace dsc
