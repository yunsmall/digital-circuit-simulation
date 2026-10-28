// 错误码与错误信息 — 被 Circuit 与 JitBackend 共用，单独成头避免互相包含
#pragma once
#include <string>
#include <vector>

namespace dsc {

enum class ErrorCode {
    OK = 0,
    PIN_NOT_FOUND,
    MULTIPLE_DRIVERS,
    COMBINATIONAL_LOOP,
    PREPARE_FAILED, // DLL 加载等编译前准备失败
    JIT_COMPILE_FAILED, // C→IR 编译失败（TCC/Clang 统一）
    JIT_LINK_FAILED, // 重定位/添加模块失败
    JIT_SYMBOL_FAILED, // 符号注入/查找失败
    BUS_CONFLICT, // 运行时总线冲突
    BUS_OSCILLATION, // 总线级联迭代到上限仍未稳定（多半是三态总线绕成的组合环）
};

struct CircuitError {
    ErrorCode code = ErrorCode::OK;
    int net_id = -1;
    std::vector<int> comp_ids; // 参与冲突的元件 ID
    std::string message;
};

} // namespace dsc
