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
// 典型生命周期（两阶段初始化）:
//
//   阶段1 — Circuit::init() 中先调用 simInit()（全局初始化）:
//     dcs_dll_init()               ← 全局一次性，加载共享资源
//
//   阶段2 — Circuit::init() 中再调用 JIT circuit_init()（实例初始化）:
//     state = dcs_dll_create()     ← 创建该实例的私有状态对象
//
//   运行期:
//     loop: dcs_dll_tick(state) / dcs_dll_reset(state)
//
//   销毁 — Circuit::deinit() 中:
//     dcs_dll_destroy(state)       ← 销毁实例状态
//     dcs_dll_deinit()             ← 全局一次性，释放共享资源
//
// 注意: dcs_dll_init() 在 dcs_dll_create() 之前调用，因此 init 中不能依赖
//       实例状态（state 尚未创建），只能做全局级别的初始化。
#pragma once

#include <stdint.h>

// 跨平台 DLL 导出宏
#ifdef _WIN32
#define DCS_DLL_EXPORT __declspec(dllexport)
#else
#define DCS_DLL_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- 引脚描述符 ----

// 单个引脚的描述
typedef struct {
    const char *name; // 引脚名称
    int bit_width; // 位宽（1~64）
    int is_tri_state; // 输出引脚：是否为三态（0=普通 1=三态）
    int is_sequential; // 输出引脚：是否依赖内部状态（0=组合 1=时序，输入引脚忽略）
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
// tick_comb: 组合逻辑阶段调用（可选，不导出则无组合部分）
// tick_seq:  时序逻辑阶段调用（可选，不导出则无时序部分）
// state / inputs / outputs 同签名
typedef void (*dcs_dll_tick_t)(void *state, const uint8_t *inputs, uint8_t *outputs);

// reset: 电路复位时调用
typedef void (*dcs_dll_reset_t)(void *state, uint8_t *outputs);

// 导出符号名宏
#define DCS_DLL_DESC "dcs_dll_desc"
#define DCS_DLL_INIT "dcs_dll_init"
#define DCS_DLL_DEINIT "dcs_dll_deinit"
#define DCS_DLL_CREATE "dcs_dll_create"
#define DCS_DLL_DESTROY "dcs_dll_destroy"
#define DCS_DLL_TICK_COMB "dcs_dll_tick_comb"
#define DCS_DLL_TICK_SEQ "dcs_dll_tick_seq"
#define DCS_DLL_RESET "dcs_dll_reset"

#ifdef __cplusplus
}
#endif
