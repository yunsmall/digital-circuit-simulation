// CodeGen — 把网表翻译成 C 源码
//
// 只读取网表（线网、元件、总线驱动关系），不修改它们，也不碰编译与运行；
// 产物是纯文本，交给 JitBackend 编译。用自由函数而非类，是为了让它天然无状态：
// 输入是网表的只读视图，输出是字符串，没有需要维护的成员。
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dcs/Circuit.h"
#include "dcs/Net.h"

namespace dsc {

class Component;
struct BusDriver;

// stop_flag_ptr: tickForever 的死循环每拍检查这个标志（总线冲突/振荡时置位）。
// 地址直接编进生成代码，和元件状态指针一样不依赖符号表——TCC 的 PE 数据符号
// 需要 dllimport 语义，走符号注入会重定位失败
std::string generateCircuitC(void *circuit_ptr,
                             volatile int *stop_flag_ptr,
                             const std::vector<std::unique_ptr<Net>> &nets,
                             const std::vector<std::unique_ptr<Component>> &components,
                             const std::map<int, std::vector<BusDriver>> &bus_nets);

std::vector<Component *> topologicalSort(const std::vector<Component *> &comb);

} // namespace dsc
