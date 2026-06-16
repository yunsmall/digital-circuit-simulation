#pragma once
#include <string>

namespace dsc {

// 获取当前可执行文件所在的目录路径
// Windows: 通过 GetModuleFileNameA 获取
// Linux:   通过 /proc/self/exe readlink 获取
// 用于运行时定位 shim 头文件目录（shim/ 在 exe 旁边或上级目录）
std::string exeDir();

} // namespace dsc
