// 存储器容量的统一入口
//
// 直接把 1 << addr_width 写在构造初始化列表里有隐患：addr_width 是 int，
// 达到 31 位时左移溢出是未定义行为，而且那时也早就超出内存承受范围。
// 这里在求值之前就拦住非法位宽。
#pragma once
#include <cstddef>
#include <stdexcept>

namespace dsc {

// 上限 30 位 = 1G 条目：单个 WishboneMemory 实例直接分配这块连续内存。
// 跑 Linux 要几百 MB，这里放到 1GB；实际容量由元件自己的 addr_width 决定，
// 不用的实例不会占这么多
inline size_t depthFromAddrWidth(int addr_width) {
    if (addr_width < 1 || addr_width > 30)
        throw std::invalid_argument("addr_width 必须在 1~30 之间");
    return size_t(1) << addr_width;
}

} // namespace dsc
