// Misc — 常量、开关、合并、拆分等杂项元件
#pragma once
#include "dcs/Component.h"

namespace dsc {

// ============================================================
// 常量元件 — 输出固定常量值，无输入
// ============================================================
class Constant : public CombinationalComponent {
public:
    // bit_width: 输出位宽（1~64）
    // value: 常量值（截断到 bit_width 位）
    Constant(const std::string &name, int bit_width, uint64_t value);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    uint64_t _value;
};

// ============================================================
// 开关元件 — 根据使能信号选择导通或断开
// ============================================================
class Switch : public CombinationalComponent {
public:
    // bit_width: 数据位宽（1~64）
    // 输入: in（数据）, en（1 位使能）
    // 输出: out（使能时 = in，否则 = 0）
    Switch(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

// ============================================================
// 合并元件 — 将 N 个 1 位信号合并为 N 位输出
// ============================================================
class Merge : public CombinationalComponent {
public:
    // num_bits: 输入位数（即输出位宽），2~64
    // 输入: in0..in_{N-1} 各 1 位
    // 输出: out（N 位），out = in0 | (in1<<1) | ...
    Merge(const std::string &name, int num_bits);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _num_bits;
};

// ============================================================
// 拆分元件 — 将 N 位输入拆分为 N 个 1 位输出
// ============================================================
class Split : public CombinationalComponent {
public:
    // num_bits: 输入位宽，2~64
    // 输入: in（N 位）
    // 输出: out0..out_{N-1} 各 1 位，out_i = (in >> i) & 1
    Split(const std::string &name, int num_bits);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _num_bits;
};

} // namespace dsc
