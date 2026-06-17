// IStorage — 存储接口
//
// 拥有内部内存的元件（Memory、ROM、FIFO、MultiPortRAM）
// 实现此接口，供外部工具 dump/load 内存数据
#pragma once
#include <cstddef>
#include <cstdint>

namespace dsc {

class IStorage {
public:
    virtual ~IStorage() = default;

    // 返回内部内存头指针（只读）
    virtual const uint8_t *memPtr() const = 0;

    // 返回内存大小（字节数）
    virtual size_t memSize() const = 0;
};

} // namespace dsc
