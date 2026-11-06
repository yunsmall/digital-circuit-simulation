// wrap_dual_clk.cpp — dual_clk 的 DLL 包装层（伪时钟：两个时钟同一拍翻转）
#include <cstring>
#include <memory>
#include "Vdual_clk.h"
#include "verilated.h"

extern "C" {
#include "dcs/dll_interface.h"
}

double sc_time_stamp() {
    return 0;
}

struct DualClkState {
    std::unique_ptr<VerilatedContext> ctx = std::make_unique<VerilatedContext>();
    Vdual_clk top{ctx.get()};
    uint8_t pseudo[2] = {};
    bool any_pseudo = false;
};

static dcs_dll_pin_desc_t s_inputs[] = {
    {"clk_a", 1, 0, 0},
    {"clk_b", 1, 0, 0},
    {"rst", 1, 0, 0},
};
static dcs_dll_pin_desc_t s_outputs[] = {
    {"count_a", 8, 0, 1},
    {"count_b", 8, 0, 1},
};

extern "C" {

DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {"dual_clk", 3, 2, s_inputs, s_outputs};

// clk_a 上升沿、clk_b 下降沿，rst 普通电平
DCS_DLL_EXPORT extern const uint8_t dcs_dll_input_edges[] = {1, 2, 0};

DCS_DLL_EXPORT void dcs_dll_set_pseudo(void *st, const uint8_t *mask) {
    auto *s = static_cast<DualClkState *>(st);
    if (!s || !mask)
        return;
    s->any_pseudo = false;
    for (int i = 0; i < 2; i++) {
        s->pseudo[i] = mask[i] != 0;
        s->any_pseudo = s->any_pseudo || s->pseudo[i];
    }
}

DCS_DLL_EXPORT void dcs_dll_init(void) {}
DCS_DLL_EXPORT void dcs_dll_deinit(void) {}

DCS_DLL_EXPORT void *dcs_dll_create(void) {
    return new DualClkState;
}

DCS_DLL_EXPORT void dcs_dll_destroy(void *st) {
    delete static_cast<DualClkState *>(st);
}

DCS_DLL_EXPORT void dcs_dll_tick_seq(void *st, const uint8_t *in, uint8_t *out) {
    auto *s = static_cast<DualClkState *>(st);
    Vdual_clk &m = s->top;
    std::memcpy(&m.clk_a, in + 0, 1);
    std::memcpy(&m.clk_b, in + 16, 1);
    std::memcpy(&m.rst, in + 32, 1);
    if (!s->any_pseudo) {
        m.eval();
    } else {
        // 两个伪时钟在同一拍统一翻转：先全部置非活跃电平（a 低、b 高），
        // eval 记录沿前状态；再全部置活跃电平（a 高、b 低），eval 产生沿
        if (s->pseudo[0]) m.clk_a = 0;
        if (s->pseudo[1]) m.clk_b = 1;
        m.eval();
        if (s->pseudo[0]) m.clk_a = 1;
        if (s->pseudo[1]) m.clk_b = 0;
        m.eval();
    }
    out[0] = (uint8_t) m.count_a;
    memset(out + 1, 0, 15);
    out[16] = (uint8_t) m.count_b;
    memset(out + 17, 0, 15);
}

DCS_DLL_EXPORT void dcs_dll_reset(void *st, uint8_t *out) {
    auto *s = static_cast<DualClkState *>(st);
    s->top.~Vdual_clk();
    s->ctx = std::make_unique<VerilatedContext>();
    new (&s->top) Vdual_clk(s->ctx.get());
    s->top.eval();
    out[0] = (uint8_t) s->top.count_a;
    memset(out + 1, 0, 15);
    out[16] = (uint8_t) s->top.count_b;
    memset(out + 17, 0, 15);
}

} // extern "C"
