#pragma once
#include <string>

// 测试工程生成的共享库文件名：CMake 在 Windows 上是 <base>.dll，
// Linux 是 lib<base>.so，macOS 是 lib<base>.dylib。测试从自己的工作目录加载
// （verilog_test/ 或构建输出目录），名字必须按平台拼，不能写死 .dll
inline std::string dllFileName(const std::string &base) {
#ifdef _WIN32
    return base + ".dll";
#elif defined(__APPLE__)
    return "lib" + base + ".dylib";
#else
    return "lib" + base + ".so";
#endif
}
