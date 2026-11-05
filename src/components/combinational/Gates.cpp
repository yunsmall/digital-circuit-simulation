#include "dcs/ComponentFactory.h"
#include "dcs/components/combinational/Gates.h"
#include <format>
#include <stdexcept>
#include <sstream>

namespace dsc {

// ============================================================
// LogicGate — 多输入逻辑门
// ============================================================
static const char *gate_op_name(GateOp op) {
    switch (op) {
        case GateOp::AND:  return "and";
        case GateOp::OR:   return "or";
        case GateOp::NAND: return "nand";
        case GateOp::NOR:  return "nor";
        case GateOp::XOR:  return "xor";
        case GateOp::XNOR: return "xnor";
    }
    return "and";
}

static const char *gate_op_str(GateOp op) {
    switch (op) {
        case GateOp::AND:  return "&";
        case GateOp::OR:   return "|";
        case GateOp::NAND: return "~(&)";
        case GateOp::NOR:  return "~(|)";
        case GateOp::XOR:  return "^";
        case GateOp::XNOR: return "~(^)";
    }
    return "&";
}

LogicGate::LogicGate(const std::string &name, int num_inputs, int bit_width, GateOp op) :
    CombinationalComponent(name, gate_op_name(op)), _bit_width(bit_width), _num_inputs(num_inputs), _op(op) {
    setParam("inputs", std::to_string(num_inputs));
    setParam("bit_width", std::to_string(bit_width));
    if (num_inputs < 2 || num_inputs > 8)
        throw std::invalid_argument(std::format("LogicGate 输入数量必须在2-8之间，当前{}", num_inputs));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    for (int i = 0; i < num_inputs; i++)
        addInput(std::format("in{}", i), bit_width);
    addOutput("out", bit_width);
}

std::string LogicGate::genFuncDef_comb() const {
    // 读取所有输入
    std::string reads;
    for (int i = 0; i < _num_inputs; i++) {
        int nid = inputs()[i]->netId(), nw = nid >= 0 ? inputs()[i]->net()->bit_width() : 0;
        reads += std::format("    {}\n", gen_read_wire(nid, _bit_width, nw, std::format("_in{}", i)));
    }

    // 生成表达式
    std::ostringstream expr;
    bool neg = (_op == GateOp::NAND || _op == GateOp::NOR || _op == GateOp::XNOR);
    if (neg)
        expr << "~(";
    const char *op_str = (_op == GateOp::AND || _op == GateOp::NAND) ? " & "
                         : (_op == GateOp::OR || _op == GateOp::NOR)  ? " | "
                         :                                                " ^ ";
    for (int i = 0; i < _num_inputs; i++) {
        if (i > 0)
            expr << op_str;
        expr << "_in" << i;
    }
    if (neg)
        // 反相在 int 上做，宽度不是 8/16/32/64 时高位会是 1；
        // 这里必须按位宽掩掉，否则脏位写进线网、下游按同宽读取时不会再次掩码
        expr << ") & " << gen_mask(_bit_width);

    return std::format(R"(static void {}(void) {{
{}
    {} _out = {};
    {}
}})",
                       funcName_comb(), reads, c_int_type(_bit_width), expr.str(),
                       genOutputWrite(0, "_out", _bit_width));
}

std::unique_ptr<Component> LogicGate::clone(const std::string &n) const {
    return std::make_unique<LogicGate>(n, _num_inputs, _bit_width, _op);
}

// ============================================================
// UnaryGate — 单输入门（NOT / BUF）
// ============================================================
UnaryGate::UnaryGate(const std::string &name, int bit_width, bool invert) :
    CombinationalComponent(name, invert ? "not" : "buf"), _bit_width(bit_width), _invert(invert) {
    setParam("bit_width", std::to_string(bit_width));
    // 线网槽是 16 字节，位宽上限 128：Release 下 addInput 的断言不生效，
    // 超过 128 会让 genOutputWrite 生成大于 16 字节的拷贝，踩坏相邻线网槽
    if (bit_width < 1 || bit_width > 128)
        throw std::invalid_argument("位宽必须在1-128之间");
    addInput("in", bit_width);
    addOutput("out", bit_width);
}

std::string UnaryGate::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;

    return std::format(R"(static void {}() {{
    {}
    {} _out = {};
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bit_width, in_nw, "_in"), c_int_type(_bit_width),
                       _invert ? std::format("~_in & {}", gen_mask(_bit_width)) : "_in",
                       genOutputWrite(0, "_out", _bit_width));
}

std::unique_ptr<Component> UnaryGate::clone(const std::string &n) const {
    return std::make_unique<UnaryGate>(n, _bit_width, _invert);
}

// ============================================================
// GateTSBUF — 三态缓冲器
// ============================================================
GateTSBUF::GateTSBUF(const std::string &name, int bit_width) :
    CombinationalComponent(name, "tsbuf"), _bit_width(bit_width) {
    setParam("bit_width", std::to_string(bit_width));
    // 同 UnaryGate：线网槽 16 字节，1..128 之外的位宽会在生成代码时越界写
    if (bit_width < 1 || bit_width > 128)
        throw std::invalid_argument("位宽必须在1-128之间");
    addInput("in", bit_width);
    addInput("oe", 1);
    addOutput("out", bit_width, true);
}

std::string GateTSBUF::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int oe_nid = inputs()[1]->netId();

    std::string read_oe = oe_nid >= 0 ? std::format("uint8_t _oe = 0; dcs_memcpy(&_oe, _w[{}], 1);", oe_nid)
                                      : "uint8_t _oe = 0;"; // 悬空按框架约定读 0：不驱动总线

    return std::format(R"(static void {}() {{
    {}
    {}
    {}
}})",
                       funcName_comb(), gen_read_wire(in_nid, _bit_width, in_nw, "_in"), read_oe,
                       genOutputWrite(0, "_in", _bit_width, "_oe"));
}

std::unique_ptr<Component> GateTSBUF::clone(const std::string &n) const {
    return std::make_unique<GateTSBUF>(n, _bit_width);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"and", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入与门，所有输入为1时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::LogicGate>(n, dsc::paramInt(p, "inputs", 2), dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::GateOp::AND);
                       });

DCS_REGISTER_COMPONENT(
    {"or", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入或门，任一输入为1时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::LogicGate>(n, dsc::paramInt(p, "inputs", 2), dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::GateOp::OR);
                       });

DCS_REGISTER_COMPONENT(
    {"nand", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入与非门，相当于与门+非门"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::LogicGate>(n, dsc::paramInt(p, "inputs", 2), dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::GateOp::NAND);
                       });

DCS_REGISTER_COMPONENT(
    {"nor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入或非门，相当于或门+非门"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::LogicGate>(n, dsc::paramInt(p, "inputs", 2), dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::GateOp::NOR);
                       });

DCS_REGISTER_COMPONENT(
    {"xor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入异或门，输入中1的个数为奇数时输出1"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::LogicGate>(n, dsc::paramInt(p, "inputs", 2), dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::GateOp::XOR);
                       });

DCS_REGISTER_COMPONENT(
    {"xnor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入同或门，相当于异或门+非门"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::LogicGate>(n, dsc::paramInt(p, "inputs", 2), dsc::paramInt(p, "bit_width", 8),
                                                                    dsc::GateOp::XNOR);
                       });

DCS_REGISTER_COMPONENT(
    {"not", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Gate, "非门/反相器，输出=~输入"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UnaryGate>(n, dsc::paramInt(p, "bit_width", 8), true);
        });

DCS_REGISTER_COMPONENT(
    {"buf", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Gate, "缓冲器，输出=输入"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UnaryGate>(n, dsc::paramInt(p, "bit_width", 8), false);
        });

DCS_REGISTER_COMPONENT(
    {"tsbuf", {{"bit_width", "8"}}, {"in", "oe"}, {"out"}, dsc::ComponentCategory::Gate, "三态缓冲器，oe=1时输出=输入，oe=0时高阻"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::GateTSBUF>(n, dsc::paramInt(p, "bit_width", 8));
                       });
} // namespace
