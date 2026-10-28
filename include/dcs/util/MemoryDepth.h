// 存储器容量的统一入口
//
// 直接把 1 << addr_width 写在构造初始化列表里有隐患：addr_width 是 int，
// 达到 31 位时左移溢出是未定义行为，而且那时也早就超出内存承受范围。
// 这里在求值之前就拦住非法位宽。
#pragma once
#include <cstddef>
#include <stdexcept>

namespace dsc {

// 上限 24 位 = 16M 条目：再大单是地址表就超过可用内存，属于配置错误而非正常用法
inline size_t depthFromAddrWidth(int addr_width) {
    if (addr_width < 1 || addr_width > 24)
        throw std::invalid_argument("addr_width 必须在 1~24 之间");
    return size_t(1) << addr_width;
}

} // namespace dsc
