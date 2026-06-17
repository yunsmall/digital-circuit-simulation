#pragma once
#include <sstream>
#include "dcs/Component.h"

namespace dsc {

// 多输入门 CRTP 基类（模板必须在头文件）
template<typename Child>
class MultiInputGate : public CombinationalComponent {
public:
    MultiInputGate(const std::string &name, const std::string &type_name, int num_inputs, int bit_width);
    int bitWidth() const {
        return _bit_width;
    }
    int numInputs() const {
        return (int) inputs().size();
    }
    std::string genFuncDef_comb() const override;

protected:
    int _bit_width;
};

// 具体门 — 声明
class GateAND : public MultiInputGate<GateAND> {
public:
    GateAND(const std::string &name, int num_inputs, int bit_width);
    static void writeExpr(std::ostringstream &oss, int n);
    std::unique_ptr<Component> clone(const std::string &n) const override;
};
class GateOR : public MultiInputGate<GateOR> {
public:
    GateOR(const std::string &name, int num_inputs, int bit_width);
    static void writeExpr(std::ostringstream &oss, int n);
    std::unique_ptr<Component> clone(const std::string &n) const override;
};
class GateNAND : public MultiInputGate<GateNAND> {
public:
    GateNAND(const std::string &name, int num_inputs, int bit_width);
    static void writeExpr(std::ostringstream &oss, int n);
    std::unique_ptr<Component> clone(const std::string &n) const override;
};
class GateNOR : public MultiInputGate<GateNOR> {
public:
    GateNOR(const std::string &name, int num_inputs, int bit_width);
    static void writeExpr(std::ostringstream &oss, int n);
    std::unique_ptr<Component> clone(const std::string &n) const override;
};
class GateXOR : public MultiInputGate<GateXOR> {
public:
    GateXOR(const std::string &name, int num_inputs, int bit_width);
    static void writeExpr(std::ostringstream &oss, int n);
    std::unique_ptr<Component> clone(const std::string &n) const override;
};
class GateXNOR : public MultiInputGate<GateXNOR> {
public:
    GateXNOR(const std::string &name, int num_inputs, int bit_width);
    static void writeExpr(std::ostringstream &oss, int n);
    std::unique_ptr<Component> clone(const std::string &n) const override;
};

class GateNOT : public CombinationalComponent {
public:
    GateNOT(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};
class GateBUF : public CombinationalComponent {
public:
    GateBUF(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};
class GateTSBUF : public CombinationalComponent {
public:
    GateTSBUF(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

} // namespace dsc

// ---- 模板实现 ----
#include <format>
#include <stdexcept>
namespace dsc {

template<typename Child>
MultiInputGate<Child>::MultiInputGate(const std::string &name, const std::string &type_name, int num_inputs,
                                      int bit_width) : CombinationalComponent(name, type_name), _bit_width(bit_width) {
    setParam("inputs", std::to_string(num_inputs));
    setParam("bit_width", std::to_string(bit_width));
    if (num_inputs < 2 || num_inputs > 8)
        throw std::invalid_argument(std::format("{}门输入数量必须在2-8之间，当前{}", type_name, num_inputs));
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    for (int i = 0; i < num_inputs; i++)
        addInput(std::format("in{}", i), bit_width);
    addOutput("out", bit_width);
}

template<typename Child>
std::string MultiInputGate<Child>::genFuncDef_comb() const {
    std::string reads;
    for (int i = 0; i < numInputs(); i++) {
        int nid = inputs()[i]->netId(), nw = nid >= 0 ? inputs()[i]->net()->bit_width() : 0;
        reads += std::format("    {}\n", gen_read_wire(nid, _bit_width, nw, std::format("_in{}", i)));
    }
    std::ostringstream expr;
    Child::writeExpr(expr, numInputs());
    int out_nid = outputs()[0]->netId();
    std::string write;
    if (out_nid >= 0) {
        write = std::format("    {}\n", genOutputWrite(0, "_out", _bit_width));
    }
    return std::format(R"(static void {}(void) {{
{}
    {} _out = {};
{}
}})",
                       funcName_comb(), reads, c_int_type(_bit_width), expr.str(), write);
}

} // namespace dsc
