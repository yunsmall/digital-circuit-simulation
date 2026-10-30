// JitBackend — 把生成的 C 源码编译成可调用代码，并解析出入口函数指针
//
// 只做"编译与符号解析"一件事：不碰网表、不生成代码、不参与 tick。
// 后端选择是编译期决定的（#ifdef DCS_USE_TCC），这里不引入虚函数——
// tick 走的是编译后解析出的函数指针，本类的方法只在外层 compile() 时调用一次。
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dcs/ErrorCode.h"

namespace dsc {

// 生成代码里的总线冲突回调（定义在 Circuit.cpp），这里只取地址注入给 JIT
void dcsBusConflictThunk(void *circuit_ptr, int net_id);

// 编译结果：成功时带出各入口函数指针，失败时带错误码与原因
struct JitCompileResult {
    ErrorCode code = ErrorCode::OK;
    int component_id = -1;
    std::string message;
    void (*tick)() = nullptr;
    // 一次调用走完一个时钟周期（低电平拍 + 高电平拍），省掉外部的两次往返
    void (*tick_cycle)(int) = nullptr;
    // 一次调用连跑 n 个周期，循环留在 JIT 里
    void (*tick_cycles)(int, int) = nullptr;
    void (*init)() = nullptr;
    void (*deinit)() = nullptr;
    void (*reset)() = nullptr;
    void (*set_wire)(int, const uint8_t *) = nullptr;
    // 标量快捷入口：高频驱动时钟这类线网时不用先拼 16 字节的缓冲区
    void (*set_wire_u64)(int, uint64_t) = nullptr;
    void (*get_wire)(int, uint8_t *) = nullptr;
};

class JitBackend {
public:
    JitBackend();
    ~JitBackend();
    JitBackend(const JitBackend &) = delete;
    JitBackend &operator=(const JitBackend &) = delete;

    // symbols 是元件通过 extraJitSymbols() 暴露给生成代码的函数指针/地址
    JitCompileResult compile(const std::string &c_code,
                             const std::string &shim_path,
                             void *circuit_ptr,
                             const std::vector<std::pair<std::string, void *>> &symbols);

    // 释放后端持有的编译产物（不卸载 DLL）
    void cleanup();

private:
    struct Impl;
    Impl *_impl = nullptr;
};

} // namespace dsc
