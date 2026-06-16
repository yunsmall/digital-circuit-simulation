#pragma once
#include "dcs/Component.h"

namespace dsc {

// ============================================================
// D 触发器 — 上升沿触发的边沿触发器
//
// 引脚:
//   d    [in]  数据输入 (bit_width)
//   clk  [in]  时钟 (1位)，上升沿触发
//   en   [in]  使能 (1位)，可选，默认为 true
//   rst  [in]  异步复位 (1位)，可选，高有效，优先级最高
//   pre  [in]  异步置位 (1位)，可选，高有效，优先级次于 rst
//   q    [out] 当前输出 (bit_width)
//
// 行为: clk 上升沿时，若 en=1 则 q ← d
//       若 rst=1 则 q ← 0（异步，不等待时钟）
//       若 pre=1 则 q ← 全 1（异步，rst 优先于 pre）
//
// 状态结构体:  q[16] + prev_clk（用于边沿检测）
// ============================================================
class DFlipFlop : public SequentialComponent {
public:
    // has_en:     是否添加 en 引脚
    // has_rst:    是否添加异步 rst 引脚
    // has_preset: 是否添加异步 pre 引脚
    DFlipFlop(const std::string &name, int bit_width, bool has_en = false, bool has_rst = false,
              bool has_preset = false);

    std::string genStructDef() const override; // typedef struct { q[16], prev_clk } _S_dff_xxx
    std::string genStateDecl() const override; // static _S_dff_xxx _s_dff_xxx;
    std::string genInitCode() const override; // memset(&_s_xxx, 0, sizeof(...))
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _has_en, _has_rst, _has_preset;
};

// ============================================================
// T 触发器 — clk 上升沿时若 t=1 则翻转，t=0 则保持
//
// 引脚:
//   t    [in]  翻转控制 (1位)
//   clk  [in]  时钟 (1位)，上升沿触发
//   q    [out] 当前输出 (bit_width)，所有位一起翻转
//
// 状态结构体: q[16] + prev_clk
// ============================================================
class TFlipFlop : public SequentialComponent {
public:
    TFlipFlop(const std::string &name, int bit_width = 1);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

// ============================================================
// JK 触发器 — clk 上升沿根据 J/K 更新输出
//
// 引脚: j [in] 1位, k [in] 1位, clk [in] 1位, q [out] bit_width
//
// 真值表（clk 上升沿）:
//   J=0 K=0: 保持    (q ← q)
//   J=0 K=1: 清零    (q ← 0)
//   J=1 K=0: 置一    (q ← 全1)
//   J=1 K=1: 翻转    (q ← ~q)
//
// 状态结构体: q[16] + prev_clk
// ============================================================
class JKFlipFlop : public SequentialComponent {
public:
    JKFlipFlop(const std::string &name, int bit_width = 1);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

// ============================================================
// 寄存器 — 多 bit D 触发器阵列，clk 上升沿锁存
//
// 引脚:
//   d    [in]  数据输入 (bit_width)
//   clk  [in]  时钟 (1位)
//   en   [in]  使能 (1位)，可选
//   rst  [in]  异步复位 (1位)，可选
//   q    [out] 寄存器输出 (bit_width)
//
// 行为: clk 上升沿时，若 en=1 则 data ← d；若 rst=1 则 data ← 0
//
// 状态结构体: data[16] + prev_clk
// ============================================================
class Register : public SequentialComponent {
public:
    Register(const std::string &name, int bit_width, bool has_en = false, bool has_async_rst = false);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _has_en, _has_rst;
};

// ============================================================
// D 锁存器 — 电平敏感的透明锁存器（非边沿触发）
//
// 引脚:
//   d    [in]  数据输入 (bit_width)
//   en   [in]  使能 (1位)
//   q    [out] 输出 (bit_width)
//
// 行为: en=1 时透明（q 跟随 d），en=0 时锁存（q 保持不变）
//       不同于 DFF，没有时钟边沿检测，en 电平直接控制
//
// 状态结构体: q[16]
// ============================================================
class Latch : public SequentialComponent {
public:
    Latch(const std::string &name, int bit_width);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
};

// ============================================================
// 计数器 — clk 上升沿递增/递减
//
// 引脚:
//   clk    [in]  时钟 (1位)，上升沿触发
//   load   [in]  同步加载信号 (1位)，可选
//   din    [in]  加载数据 (bit_width)，可选（load=1 时生效）
//   en     [in]  计数使能 (1位)，可选，默认为 true
//   updown [in]  方向 (1位)，可选，0=加 1=减
//   clr    [in]  异步清零 (1位)，可选，高有效
//   q      [out] 计数值 (bit_width)
//
// 行为（clk 上升沿）:
//   若 load=1: count ← din（同步加载）
//   否则若 en=1:
//     若 updown=1: count ← count - 1
//     否则:        count ← count + 1
//   若 clr=1: count ← 0（异步，不等待时钟）
//
// 状态结构体: count[16] + prev_clk
// ============================================================
class Counter : public SequentialComponent {
public:
    Counter(const std::string &name, int bit_width, bool has_load = false, bool has_en = false, bool has_updown = false,
            bool has_async_clr = false);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _bit_width;
    bool _has_load, _has_en, _has_updown, _has_clr;
};

// ============================================================
// 移位寄存器 — 串行输入，并行输出
//
// 引脚:
//   sin   [in]  串行输入 (1位)
//   clk   [in]  时钟 (1位)，上升沿触发
//   dir   [in]  方向 (1位)，可选，0=左移(sin→LSB) 1=右移(sin→MSB)
//   clr   [in]  异步清零 (1位)，可选
//   q     [out] 并行输出 (length 位)
//
// 行为（clk 上升沿）:
//   左移: 所有位左移一位，LSB 填入 sin，MSB 移出丢弃
//   右移: 所有位右移一位，MSB 填入 sin，LSB 移出丢弃
//   若 clr=1: 所有位清零（异步）
//
// 状态结构体: stages[16] + prev_clk
// ============================================================
class ShiftRegister : public SequentialComponent {
public:
    // length: 移位寄存器级数（1~64），即输出位宽
    ShiftRegister(const std::string &name, int length, bool has_dir = false, bool has_async_clr = false);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _length;
    bool _has_dir, _has_clr;
};

// ============================================================
// ClockGen — 可配置占空比的时钟发生器
//
// 输出 clk 周期方波：高电平持续 high_ticks 周期，低电平持续 low_ticks 周期。
// 无输入，仅输出 clk（1 位）
// ============================================================
class ClockGen : public SequentialComponent {
public:
    // high_ticks: 高电平持续的 tick 数（默认 1）
    // low_ticks:  低电平持续的 tick 数（默认 1）
    // 周期 = high_ticks + low_ticks
    ClockGen(const std::string &name, int high_ticks = 1, int low_ticks = 1);
    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode()  const override;
    std::string genFuncDef()   const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _high, _low, _period;
};

} // namespace dsc
