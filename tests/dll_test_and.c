// dll_test_and.c — 测试用 DLL 元件：8 位 AND 门
//
// 编译（MSVC）:
//   cl /LD tests/dll_test_and.c /Iinclude /Fe:build/RelWithDebInfo/dll_test_and.dll
//
// 导出: dcs_dll_desc, dcs_dll_init, dcs_dll_deinit,
//       dcs_dll_create, dcs_dll_destroy, dcs_dll_tick_comb, dcs_dll_reset

#include <stdlib.h>
#include <string.h>
#include "dcs/dll_interface.h"

// ---- 状态（AND 门无内部状态，仅示例） ----
typedef struct {
    int dummy;
} AndState;

// ---- 引脚描述 ----
static dcs_dll_pin_desc_t s_inputs[] = {{"a", 8, 0, 0}, {"b", 8, 0, 0}};
static dcs_dll_pin_desc_t s_outputs[] = {{"y", 8, 0, 0}};

DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {"and_gate", 2, 1, s_inputs, s_outputs};

// ---- 生命周期 ----
DCS_DLL_EXPORT void dcs_dll_init(void) {
}
DCS_DLL_EXPORT void dcs_dll_deinit(void) {
}

DCS_DLL_EXPORT void *dcs_dll_create(void) {
    AndState *s = (AndState *) malloc(sizeof(AndState));
    s->dummy = 0;
    return s;
}

DCS_DLL_EXPORT void dcs_dll_destroy(void *st) {
    free(st);
}

// ---- 仿真逻辑 ----

// 每个引脚在缓冲区中占 16 字节（线网格式 uint8_t[16]）
// 引脚顺序与描述符中一致
DCS_DLL_EXPORT void dcs_dll_tick_comb(void *st, const uint8_t *inputs, uint8_t *outputs) {
    (void) st;
    // inputs:  0..15 = a, 16..31 = b（每个引脚 16 字节）
    // outputs: 0..15 = y
    uint8_t a = inputs[0]; // a 的值（第 1 字节，高位为 0）
    uint8_t b = inputs[16]; // b 的值
    outputs[0] = a & b;
    memset(outputs + 1, 0, 15); // 清零高位
}

DCS_DLL_EXPORT void dcs_dll_reset(void *st, uint8_t *outputs) {
    (void) st;
    memset(outputs, 0, 16);
}
