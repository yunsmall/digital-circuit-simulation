// wrap_reg8.cpp — 组合+时序混合示例
// 输出 q(时序) 和 d_out(组合)，tick_seq 需要同时写两个输出
//
// 每个元件实例独立持有 VerilatedContext，多个同 DLL 元件互不干扰。
#include <cstring>
#include <memory>
#include "Vreg8.h"
#include "verilated.h"

extern "C" {
#include "dcs/dll_interface.h"
}

double sc_time_stamp() {
    return 0;
}

// ---- 每个实例独立的 Verilator 状态 ----
struct Reg8State {
    std::unique_ptr<VerilatedContext> ctx = std::make_unique<VerilatedContext>();
    Vreg8 top{ctx.get()};
};

// 输入: clk(1) d(8)  输出: q(8,时序) d_out(8,组合)
static dcs_dll_pin_desc_t s_inputs[] = {
    {"clk", 1, 0, 0},
    {"d", 8, 0, 0},
};
static dcs_dll_pin_desc_t s_outputs[] = {
    {"q", 8, 0, 1},    // 时序
    {"d_out", 8, 0, 0}, // 组合
};

extern "C" {

DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {"reg8", 2, 2, s_inputs, s_outputs};

DCS_DLL_EXPORT void dcs_dll_init(void) {}
DCS_DLL_EXPORT void dcs_dll_deinit(void) {}

DCS_DLL_EXPORT void *dcs_dll_create(void) {
    return new Reg8State;
}

DCS_DLL_EXPORT void dcs_dll_destroy(void *st) {
    delete static_cast<Reg8State *>(st);
}

// 组合阶段
DCS_DLL_EXPORT void dcs_dll_tick_comb(void *st, const uint8_t *inputs, uint8_t *outputs) {
    auto *s = static_cast<Reg8State *>(st);
    s->top.clk = 0;
    s->top.d = inputs[16];
    s->top.eval();
    outputs[16] = s->top.d_out;
    memset(outputs + 17, 0, 15);
}

// 时序阶段（同时写两个输出，因为 DllComponent 在 seq 阶段写全部输出）
DCS_DLL_EXPORT void dcs_dll_tick_seq(void *st, const uint8_t *inputs, uint8_t *outputs) {
    auto *s = static_cast<Reg8State *>(st);
    s->top.clk = inputs[0] & 1;
    s->top.d = inputs[16];
    s->top.eval();
    // 时序输出
    outputs[0] = s->top.q;
    memset(outputs + 1, 0, 15);
    // 组合输出
    outputs[16] = s->top.d_out;
    memset(outputs + 17, 0, 15);
}

DCS_DLL_EXPORT void dcs_dll_reset(void *st, uint8_t *outputs) {
    auto *s = static_cast<Reg8State *>(st);
    s->top.clk = 0;
    s->top.d = 0;
    s->top.eval();
    outputs[0] = s->top.q;
    outputs[16] = s->top.d_out;
    memset(outputs + 1, 0, 15);
    memset(outputs + 17, 0, 15);
}

} // extern "C"
