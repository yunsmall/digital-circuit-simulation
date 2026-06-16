// dcs_dll_interface.h — 外部 DLL 元件接口定义
//
// 编写自定义 DLL 元件时，需要导出以下 C 链接函数和描述结构体。
// DllComponent 加载 DLL 后自动根据描述符创建引脚、生成 JIT 调用代码。
//
// 必须导出的符号:
//
//   [描述符]  dcs_dll_desc               — dcs_dll_descriptor_t 全局变量
//                                         （描述引脚名称、数量、位宽）
//
//   [生命周期] dcs_dll_init(void)         — 仿真启动（全局一次性）
//              dcs_dll_deinit(void)       — 仿真结束（全局一次性）
//              dcs_dll_create(void) → void* — 创建状态对象
//              dcs_dll_destroy(void*)     — 销毁状态对象
//
//   [仿真逻辑] dcs_dll_tick(void*)        — 每个时钟周期调用
//              dcs_dll_reset(void*)       — 电路复位时调用
//
// 典型生命周期:
//   dcs_dll_init()
//   state = dcs_dll_create()
//   loop: dcs_dll_tick(state) / dcs_dll_reset(state)
//   dcs_dll_destroy(state)
//   dcs_dll_deinit()
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 引脚描述符 ----

// 单个引脚的描述
typedef struct {
    const char *name; // 引脚名称
    int bit_width; // 位宽（1~64）
} dcs_dll_pin_desc_t;

// 元件描述符（DLL 必须导出名为 dcs_dll_desc 的全局变量）
typedef struct {
    const char *type_name; // 元件类型名（如 "my_filter"）
    int num_inputs; // 输入引脚数量
    int num_outputs; // 输出引脚数量
    dcs_dll_pin_desc_t *inputs; // 输入引脚数组（长度 num_inputs）
    dcs_dll_pin_desc_t *outputs; // 输出引脚数组（长度 num_outputs）
} dcs_dll_descriptor_t;

// ---- 仿真生命周期 ----
typedef void (*dcs_dll_init_t)(void);
typedef void (*dcs_dll_deinit_t)(void);

// ---- 状态对象 ----
typedef void *(*dcs_dll_create_t)(void);
typedef void (*dcs_dll_destroy_t)(void *state);

// ---- 仿真逻辑 ----
// tick: 每个时钟周期调用
//   state   — 状态对象
//   inputs  — 输入引脚数据（num_inputs × 16 字节，顺序与描述符一致）
//   outputs — 输出引脚缓冲区（num_outputs × 16 字节，由 DLL 填入）
typedef void (*dcs_dll_tick_t)(void *state, const uint8_t *inputs, uint8_t *outputs);

// reset: 电路复位时调用
//   可以在此设置 outputs 的初始值
typedef void (*dcs_dll_reset_t)(void *state, uint8_t *outputs);

// 导出符号名宏
#define DCS_DLL_DESC "dcs_dll_desc"
#define DCS_DLL_INIT "dcs_dll_init"
#define DCS_DLL_DEINIT "dcs_dll_deinit"
#define DCS_DLL_CREATE "dcs_dll_create"
#define DCS_DLL_DESTROY "dcs_dll_destroy"
#define DCS_DLL_TICK "dcs_dll_tick"
#define DCS_DLL_RESET "dcs_dll_reset"

#ifdef __cplusplus
}
#endif
