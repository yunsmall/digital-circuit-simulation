// FloatPoint — IEEE 754 单/双精度浮点运算元件
//
// 输入/输出位宽固定：float32=32位，float64=64位
// 内部使用 C 的 float/double 类型计算，通过 memcpy 与线网交换数据
#pragma once
#include "dcs/Component.h"

namespace dsc {

// ============================================================
// 浮点加法 — out = a + b
// ============================================================
class FloatAdd : public CombinationalComponent {
public:
    // precision: 32 (float) 或 64 (double)，默认 32
    FloatAdd(const std::string &name, int precision = 32);
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
};

// ============================================================
// 浮点减法 — out = a - b
// ============================================================
class FloatSub : public CombinationalComponent {
public:
    FloatSub(const std::string &name, int precision = 32);
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
};

// ============================================================
// 浮点乘法 — out = a * b
// ============================================================
class FloatMul : public CombinationalComponent {
public:
    FloatMul(const std::string &name, int precision = 32);
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
};

// ============================================================
// 浮点除法 — out = a / b
// ============================================================
class FloatDiv : public CombinationalComponent {
public:
    FloatDiv(const std::string &name, int precision = 32);
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
};

// ============================================================
// 浮点比较 — out = (a op b) ? 1 : 0
// ============================================================
enum class FloatCmpOp { EQ, NE, LT, GT, LE, GE };

class FloatCmp : public CombinationalComponent {
public:
    FloatCmp(const std::string &name, int precision, FloatCmpOp op);
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
    FloatCmpOp _op;
};

// ============================================================
// 浮点常量 — 输出固定浮点值（无输入）
// ============================================================
class FloatConst : public CombinationalComponent {
public:
    // precision: 32 (float) 或 64 (double)
    // value: 浮点值（double 表示，内部按 precision 截断）
    FloatConst(const std::string &name, int precision, double value);
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
    double _value;
};

} // namespace dsc
