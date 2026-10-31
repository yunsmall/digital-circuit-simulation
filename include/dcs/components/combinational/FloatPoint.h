// FloatPoint — IEEE 754 单/双精度浮点运算元件
//
// 输入/输出位宽固定：float32=32位，float64=64位
// 内部使用 C 的 float/double 类型计算，通过 memcpy 与线网交换数据
#pragma once
#include "dcs/Component.h"

namespace dsc {

// ============================================================
// FloatBinOp — 浮点二元运算（加/减/乘/除）
//
// 替代原有 FloatAdd/FloatSub/FloatMul/FloatDiv 四个类
// ============================================================
enum class FloatBinOpKind { ADD, SUB, MUL, DIV };

class FloatBinOp : public CombinationalComponent {
public:
    FloatBinOp(const std::string &name, int precision, FloatBinOpKind op);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
    FloatBinOpKind _op;
};

// ============================================================
// 浮点比较 — out = (a op b) ? 1 : 0
// ============================================================
enum class FloatCmpOp { EQ, NE, LT, GT, LE, GE };

class FloatCmp : public CombinationalComponent {
public:
    FloatCmp(const std::string &name, int precision, FloatCmpOp op);
    std::string genFuncDef_comb() const override;
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
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _precision;
    double _value;
};

// ============================================================
// 浮点 → 整型转换 — 截断取整，浮点范围外行为未定义
//
// 引脚:
//   in   [in]  浮点输入（32 或 64 位）
//   out  [out] 整型输出（位宽由 int_width 指定，1~64）
//
// 有符号: float → int64_t → 掩码 → 输出
// 无符号: float → uint64_t → 掩码 → 输出
// ============================================================
class FloatToInt : public CombinationalComponent {
public:
    // fp: 浮点位宽（32=float, 64=double）
    // int_width: 整型输出位宽（1~64）
    // s: 有符号整型输出？
    FloatToInt(const std::string &name, int fp, int int_width, bool s = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _fp, _int_w;
    bool _signed;
};

// ============================================================
// 整型 → 浮点转换
//
// 引脚:
//   in   [in]  整型输入（位宽 int_width，1~64）
//   out  [out] 浮点输出（32 或 64 位）
//
// 有符号: N 位整型 → 符号扩展至 int64_t → 强转 float/double
// 无符号: N 位整型 → 零扩展 → 强转 float/double
// ============================================================
class IntToFloat : public CombinationalComponent {
public:
    // int_width: 整型输入位宽（1~64）
    // fp: 浮点输出位宽（32=float, 64=double）
    // s: 输入为有符号整型？
    IntToFloat(const std::string &name, int int_width, int fp, bool s = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _int_w, _fp;
    bool _signed;
};

} // namespace dsc
