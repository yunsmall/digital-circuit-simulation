#pragma once
#include "dcs/Component.h"

namespace dsc {

// ============================================================
// GateOp — 多输入逻辑门操作类型
// ============================================================
enum class GateOp { AND, OR, NAND, NOR, XOR, XNOR };

// ============================================================
// LogicGate — 多输入逻辑门（2~8 输入）
//
// 替代原有 GateAND/OR/NAND/NOR/XOR/XNOR 六个类
// ============================================================
class LogicGate : public CombinationalComponent {
public:
    LogicGate(const std::string &name, int num_inputs, int bit_width, GateOp op);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    int bitWidth() const { return _bit_width; }
    int numInputs() const { return _num_inputs; }

private:
    int _bit_width;
    int _num_inputs;
    GateOp _op;
};

// ============================================================
// UnaryGate — 单输入门（NOT / BUF）
//
// invert=true → NOT（取反），invert=false → BUF（保持）
// ============================================================
class UnaryGate : public CombinationalComponent {
public:
    UnaryGate(const std::string &name, int bit_width, bool invert = true);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _invert;
};

// ============================================================
// GateTSBUF — 三态缓冲器
// ============================================================
class GateTSBUF : public CombinationalComponent {
public:
    GateTSBUF(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

} // namespace dsc
