// Context — 一次加载/仿真所用的上下文。
//
// 和 LLVM 的 LLVMContext 一样显式传递：可变状态（蓝图库、JIT 头文件目录）放在这里，
// 需要同时跑多套互不相关的电路时各用各的 Context，不靠进程级单例互相踩。
//
// 元件类型注册表（ComponentFactory）不在这里：类型登记是进程级的，自注册在静态
// 初始化阶段完成，和"这次加载了哪些蓝图"无关。
#pragma once
#include <string>
#include <utility>

#include "dcs/CircuitLibrary.h"

namespace dsc {

class Context {
public:
    CircuitLibrary &library() {
        return _library;
    }
    const CircuitLibrary &library() const {
        return _library;
    }

    // JIT 编译时生成代码要 include 的 shim 目录；空表示按可执行文件位置自动检测
    void setJitIncludeDir(std::string dir) {
        _jit_include_dir = std::move(dir);
    }
    [[nodiscard]] const std::string &jitIncludeDir() const {
        return _jit_include_dir;
    }

    // 进程级默认上下文：不显式指定 Context 的老代码（Circuit 默认构造）都用它
    static Context &global();

private:
    CircuitLibrary _library;
    std::string _jit_include_dir;
};

} // namespace dsc
