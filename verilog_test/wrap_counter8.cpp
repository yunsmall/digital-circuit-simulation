// wrap_counter8.cpp — Verilator 生成的 counter8 的 C 包装（时序）
//
// 每个元件实例独立持有 VerilatedContext，多个同 DLL 元件互不干扰。
#include <cstring>
#include <memory>
#include "Vcounter8.h"
#include "verilated.h"

extern "C" {
#include "dcs/dll_interface.h"
}

double sc_time_stamp() {
    return 0;
}

// ---- 每个实例独立的 Verilator 状态 ----
struct Counter8State {
    std::unique_ptr<VerilatedContext> ctx = std::make_unique<VerilatedContext>();
    Vcounter8 top{ctx.get()};
    // 接在伪时钟线上时，clk 输入恒 0，模型看不到任何跳变，由这里每拍合成
    bool pseudo_clk = false;
};

static dcs_dll_pin_desc_t s_inputs[] = {
    {"clk", 1, 0, 0},
    {"rst", 1, 0, 0},
    {"en", 1, 0, 0},
};
static dcs_dll_pin_desc_t s_outputs[] = {
    {"count", 8, 0, 1}, // is_sequential=1，参加时序阶段
};

extern "C" {

DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {"counter8", 3, 1, s_inputs, s_outputs};

// clk 在上沿触发；rst/en 是电平输入。数组顺序和描述符一致。
// C++ 的 const 全局默认内部链接，必须写 extern 才能导出
DCS_DLL_EXPORT extern const uint8_t dcs_dll_input_edges[] = {1, 0, 0};

// dcs 在编译电路时告知哪些输入接伪时钟线。这个模型只有 clk 可能是伪时钟
DCS_DLL_EXPORT void dcs_dll_set_pseudo(void *st, const uint8_t *mask) {
    if (st && mask)
        static_cast<Counter8State *>(st)->pseudo_clk = mask[0] != 0;
}

DCS_DLL_EXPORT void dcs_dll_init(void) {}
DCS_DLL_EXPORT void dcs_dll_deinit(void) {}

DCS_DLL_EXPORT void *dcs_dll_create(void) {
    return new Counter8State;
}

DCS_DLL_EXPORT void dcs_dll_destroy(void *st) {
    delete static_cast<Counter8State *>(st);
}

// 时序 tick
DCS_DLL_EXPORT void dcs_dll_tick_seq(void *st, const uint8_t *inputs, uint8_t *outputs) {
    auto *s = static_cast<Counter8State *>(st);
    s->top.rst = inputs[16] & 1;
    s->top.en = inputs[32] & 1;
    if (!s->pseudo_clk) {
        s->top.clk = inputs[0] & 1;
        s->top.eval();
    } else {
        // 伪时钟线电平恒 0：先置低 eval 记录沿前状态，再置高 eval 产生上升沿
        s->top.clk = 0;
        s->top.eval();
        s->top.clk = 1;
        s->top.eval();
    }
    outputs[0] = s->top.count;
    memset(outputs + 1, 0, 15);
}

DCS_DLL_EXPORT void dcs_dll_reset(void *st, uint8_t *outputs) {
    auto *s = static_cast<Counter8State *>(st);
    s->top.clk = 0;
    s->top.rst = 1;
    s->top.en = 0;
    s->top.eval();
    outputs[0] = s->top.count;
    memset(outputs + 1, 0, 15);
}

} // extern "C"
