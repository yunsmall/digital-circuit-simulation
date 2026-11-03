#pragma once
#include <format>
#include <string>

namespace dsc::cgen {

// JIT 入口函数名：CodeGen 生成和 JitBackend 解析必须用同一份字符串，
// 否则新增/改名入口时只在运行期表现为找不到符号
inline constexpr const char *kTickFn = "circuit_tick";
inline constexpr const char *kTickCycleFn = "circuit_tick_cycle";
inline constexpr const char *kTickCyclesFn = "circuit_tick_cycles";
inline constexpr const char *kTickBatchFn = "circuit_tick_batch";
inline constexpr const char *kTickForeverFn = "circuit_tick_forever";
inline constexpr const char *kInitFn = "circuit_init";
inline constexpr const char *kDeinitFn = "circuit_deinit";
inline constexpr const char *kResetFn = "circuit_reset";
inline constexpr const char *kSetWireFn = "circuit_set_wire";
inline constexpr const char *kSetWireU64Fn = "circuit_set_wire_u64";
inline constexpr const char *kGetWireFn = "circuit_get_wire";

// 生成 C 代码里的全局命名只在这里拼。CodeGen 负责声明、元件负责使用，
// 同一条规则散在两边时，改前缀/改布局漏一处就是运行期读到别的静态变量
inline std::string wire(int net_id) {
    return std::format("_w[{}]", net_id);
}

inline std::string cOut(int comp_id, int out_idx) {
    return std::format("_c_out_{}_{}", comp_id, out_idx);
}

inline std::string cOe(int comp_id, int out_idx) {
    return std::format("_c_oe_{}_{}", comp_id, out_idx);
}

} // namespace dsc::cgen
