// wrap_adder8.cpp — Verilator 生成的 adder8 的 C 包装
//
// 每个元件实例独立持有 VerilatedContext，多个同 DLL 元件互不干扰。
#include "Vadder8.h"
#include "dcs/dll_interface.h"
#include "verilated.h"
#include <memory>

// ---- SystemC 桩 ----
double sc_time_stamp() {
    return 0;
}

// ---- 每个实例独立的 Verilator 状态 ----
struct Adder8State {
    std::unique_ptr<VerilatedContext> ctx = std::make_unique<VerilatedContext>();
    Vadder8 top{ctx.get()};
};

// ---- 引脚描述 ----
static dcs_dll_pin_desc_t s_inputs[] = {
    {"a", 8, 0, 0},
    {"b", 8, 0, 0},
    {"cin", 1, 0, 0},
};
static dcs_dll_pin_desc_t s_outputs[] = {
    {"sum", 8, 0, 0},
    {"cout", 1, 0, 0},
};

extern "C" {

DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {"adder8", 3, 2, s_inputs, s_outputs};

DCS_DLL_EXPORT void dcs_dll_init(void) {}
DCS_DLL_EXPORT void dcs_dll_deinit(void) {}

DCS_DLL_EXPORT void *dcs_dll_create(void) {
    return new Adder8State;
}

DCS_DLL_EXPORT void dcs_dll_destroy(void *st) {
    delete static_cast<Adder8State *>(st);
}

// 引脚在缓冲区中各占 16 字节（线网格式 uint8_t[16]）
// 输入顺序: a[16] b[16] cin[16]  输出顺序: sum[16] cout[16]
DCS_DLL_EXPORT void dcs_dll_tick_comb(void *st, const uint8_t *inputs, uint8_t *outputs) {
    auto *s = static_cast<Adder8State *>(st);
    s->top.a = inputs[0];
    s->top.b = inputs[16];
    s->top.cin = inputs[32] & 1;
    s->top.eval();
    outputs[0] = s->top.sum;
    memset(outputs + 1, 0, 15);
    outputs[16] = s->top.cout & 1;
    memset(outputs + 17, 0, 15);
}

DCS_DLL_EXPORT void dcs_dll_reset(void *st, uint8_t *outputs) {
    auto *s = static_cast<Adder8State *>(st);
    s->top.a = 0;
    s->top.b = 0;
    s->top.cin = 0;
    s->top.eval();
    outputs[0] = s->top.sum;
    memset(outputs + 1, 0, 15);
    outputs[16] = s->top.cout & 1;
    memset(outputs + 17, 0, 15);
}

} // extern "C"
