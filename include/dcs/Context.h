// Context — 一次加载/仿真所用的上下文。
//
// 和 LLVM 的 LLVMContext 一样显式传递：可变状态（蓝图库、JIT 头文件目录）放在这里，
// 需要同时跑多套互不相关的电路时各用各的 Context，不靠进程级单例互相踩。
//
// 元件类型注册表（ComponentFactory）不在这里：类型登记是进程级的，自注册在静态
// 初始化阶段完成，和"这次加载了哪些蓝图"无关。
#pragma once
#include <filesystem>
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

    // 当前正在加载的 JSON/蓝图所在目录。元件参数里的相对路径由元件自己
    // （或它选定的后端）用 resolvePath 解析，不需要加载器知道"哪些参数是路径"
    void setSourceDir(std::string dir) {
        _source_dir = std::move(dir);
    }
    [[nodiscard]] const std::string &sourceDir() const {
        return _source_dir;
    }
    // 相对路径按 sourceDir 展开；绝对路径原样返回。sourceDir 为空时按工作目录
    [[nodiscard]] std::string resolvePath(const std::string &p) const {
        if (p.empty())
            return p;
        const std::filesystem::path path(p);
        if (path.is_absolute())
            return p;
        const std::filesystem::path base =
                _source_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(_source_dir);
        return (base / path).string();
    }

    // 进入一个文件/定义时临时切换 sourceDir，离开时恢复
    class ScopedSourceDir {
    public:
        ScopedSourceDir(Context &ctx, std::string dir) : _ctx(ctx), _old(ctx.sourceDir()) {
            _ctx.setSourceDir(std::move(dir));
        }
        ~ScopedSourceDir() {
            _ctx.setSourceDir(std::move(_old));
        }
        ScopedSourceDir(const ScopedSourceDir &) = delete;
        ScopedSourceDir &operator=(const ScopedSourceDir &) = delete;

    private:
        Context &_ctx;
        std::string _old;
    };

    // 进程级默认上下文：不显式指定 Context 的老代码（Circuit 默认构造）都用它
    static Context &global();

private:
    CircuitLibrary _library;
    std::string _jit_include_dir;
    std::string _source_dir;
};

} // namespace dsc
