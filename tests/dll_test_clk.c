// dll_test_clk.c — 测试用 DLL 元件：时钟沿计数
//
// 验证两件事：
//   1. DLL 可以导出 dcs_dll_input_edges，让 DllComponent 按边沿依赖建引脚；
//   2. DLL 可以导出 dcs_dll_set_pseudo，由运行时告诉它哪些输入是"每 tick
//      一个沿"的伪时钟线（这些输入的电平恒 0，没有合成能力的 DLL 会永远
//      看不到时钟）。
//
// 定义 DCS_TEST_NO_SET_PSEUDO 编出来的副本故意不导出 dcs_dll_set_pseudo，
// 用来测试"声明了边沿却接伪时钟"必须在编译前报错。
#include <stdlib.h>
#include <string.h>
#include "dcs/dll_interface.h"

typedef struct {
    uint8_t count;
    uint8_t prev_clk;
    uint8_t pseudo_clk;
} ClkState;

// 引脚描述：clk 是 1 位上升沿，rst 是 1 位下降沿，count 是 8 位时序输出
static dcs_dll_pin_desc_t s_inputs[] = {{"clk", 1, 0, 0}, {"rst", 1, 0, 0}};
static dcs_dll_pin_desc_t s_outputs[] = {{"count", 8, 0, 1}};

DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {"clk_counter", 2, 1, s_inputs, s_outputs};

// 和描述符一一对应：0 无、1 上升、2 下降、3 任意变化
DCS_DLL_EXPORT const uint8_t dcs_dll_input_edges[] = {1, 2};

DCS_DLL_EXPORT void dcs_dll_init(void) {}
DCS_DLL_EXPORT void dcs_dll_deinit(void) {}

DCS_DLL_EXPORT void *dcs_dll_create(void) {
    ClkState *s = (ClkState *) malloc(sizeof(ClkState));
    if (s)
        memset(s, 0, sizeof(*s));
    return s;
}

DCS_DLL_EXPORT void dcs_dll_destroy(void *st) {
    free(st);
}

DCS_DLL_EXPORT void dcs_dll_reset(void *st, uint8_t *outputs) {
    ClkState *s = (ClkState *) st;
    if (s) {
        s->count = 0;
        s->prev_clk = 0;
    }
    memset(outputs, 0, 16);
}

#ifndef DCS_TEST_NO_SET_PSEUDO
// mask[i] = 1 表示第 i 个输入接在伪时钟线上。这个元件只关心 clk（下标 0）
DCS_DLL_EXPORT void dcs_dll_set_pseudo(void *st, const uint8_t *mask) {
    ClkState *s = (ClkState *) st;
    if (s)
        s->pseudo_clk = (mask && mask[0]) ? 1 : 0;
}
#endif

DCS_DLL_EXPORT void dcs_dll_tick_seq(void *st, const uint8_t *in, uint8_t *out) {
    ClkState *s = (ClkState *) st;
    int rise;
    if (s->pseudo_clk) {
        // 伪时钟每个 tick 都是一个上升沿，输入电平没有意义
        rise = 1;
    } else {
        uint8_t clk = in[0] & 1;
        rise = clk && !s->prev_clk;
        s->prev_clk = clk;
    }
    if (rise)
        s->count++;
    memset(out, 0, 16);
    out[0] = s->count;
}
