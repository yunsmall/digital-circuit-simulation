#pragma once
#include "dcs/Component.h"

namespace dsc {

// ============================================================
// 多路选择器 — 从 2^n 个输入端中选出一个输出
//
// 选择线数量 n_selects（1~4）决定输入端数量（2/4/8/16）
// 所有数据端共享相同的位宽 bit_width（1~64）
//
// 生成 C 代码：根据 sel 位组合成索引，if-else 链选通对应输入
// ============================================================
class Mux : public CombinationalComponent {
public:
    // n_selects: 选择线数量（1~4）
    // bit_width: 数据位宽（1~64）
    Mux(const std::string &name, int n_selects, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _n_selects, _bit_width;
};

// ============================================================
// 加法器 — 全加器：{cout, sum} = a + b + cin
//
// 输入端 a、b 为 bit_width 位宽，cin 为 1 位进位输入
// 输出端 sum 为 bit_width 位宽，cout 为 1 位进位输出
//
// 实现：用 uint64_t 扩展精度计算完整和值，截断低 bit_width 位为 sum，超出部分为 cout
// ============================================================
class Adder : public CombinationalComponent {
public:
    // bit_width: 操作数位宽（1~64）
    // is_signed: 有符号模式，使用 int64_t 内部计算
    Adder(const std::string &name, int bit_width, bool is_signed = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _signed;
};

// ============================================================
// 比较器 — 比较两个数的关系，输出 1 位结果
//
// 支持六种比较：EQ(相等)、NE(不等)、LT(小于)、GT(大于)、LE(小于等于)、GE(大于等于)
// 输入端 a、b 为 bit_width 位宽，输出端 out 为 1 位
// ============================================================
enum class CmpOp { EQ, NE, LT, GT, LE, GE };

class Comparator : public CombinationalComponent {
public:
    // bit_width: 操作数位宽（1~64）
    // op: 比较运算类型
    // is_signed: 有符号比较模式（对 LT/GT/LE/GE 有影响，EQ/NE 无影响）
    Comparator(const std::string &name, int bit_width, CmpOp op, bool is_signed = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    CmpOp _op;
    bool _signed;
};

// ============================================================
// 译码器 — N 位二进制选择 → 2^N 位 one-hot 输出
//
// 输入端 sel[0..n-1] 各为 1 位，组成 N 位二进制选择值
// 输出端 out[0..2^n-1] 各为 1 位，仅与选择值对应的输出为 1，其余为 0
//
// 生成 C 代码：将 sel 位拼接为整数索引，逐输出比较
// ============================================================
class Decoder : public CombinationalComponent {
public:
    // n_selects: 选择位宽（1~8），产生 2^n 个输出
    Decoder(const std::string &name, int n_selects);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _n_selects;
};

// ============================================================
// 编码器 — 2^N 位 one-hot 输入 → N 位二进制输出（译码器反操作）
//
// 输入端 in[0..2^n-1] 各为 1 位（one-hot）
// 输出端 out[0..n-1] 各为 1 位，拼成 N 位二进制值
// 多输入同时为 1 时行为未定义（输出最高优先级的位）
// ============================================================
class Encoder : public CombinationalComponent {
public:
    // n_selects: 二进制输出位宽（1~8），需要 2^n 个输入
    Encoder(const std::string &name, int n_selects);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _n_selects;
};

// ============================================================
// 减法器 — {bout, diff} = a - b - bin
//
// 输入端 a、b 为 bit_width 位宽，bin 为 1 位借位输入
// 输出端 diff 为 bit_width 位宽，bout 为 1 位借位输出
// ============================================================
class Subtractor : public CombinationalComponent {
public:
    Subtractor(const std::string &name, int bit_width);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

// ============================================================
// 乘法器 — prod = a * b（有符号/无符号）
//
// 输入端 a、b 各为 bit_width 位宽
// 输出端 prod 为 2*bit_width 位宽
// is_signed: true=有符号乘法（符号扩展后相乘），false=无符号
// ============================================================
class Multiplier : public CombinationalComponent {
public:
    Multiplier(const std::string &name, int bit_width, bool is_signed = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _signed;
};

// ============================================================
// 除法器 — {quot, rem} = a / b（有符号/无符号）
//
// 输入端 a（被除数）、b（除数）各为 bit_width 位宽
// 输出端 quot（商）、rem（余数）各为 bit_width 位宽
// b=0 时 quot=rem=0（除零保护）
// ============================================================
class Divider : public CombinationalComponent {
public:
    Divider(const std::string &name, int bit_width, bool is_signed = false);
    std::string genFuncDef_comb() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _signed;
};

} // namespace dsc
