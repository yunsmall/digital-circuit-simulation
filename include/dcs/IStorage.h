// IStorage — 存储接口
//
// 拥有内部紧凑内存的元件（Memory、ROM、FIFO、MultiPortRAM）实现此接口，
// 供外部工具直接 dump/load 内存数据。
//
// 用法示例：
//   auto *s = dynamic_cast<IStorage*>(comp);
//   // 读取
//   const uint8_t *raw = s->readOnlyMemPtr();
//   size_t n = s->memSize();
//   // 写入（不可写返回 nullptr）
//   uint8_t *wp = s->writableMemPtr();
//   if (wp) std::memcpy(wp, my_data, std::min(sizeof(my_data), s->memSize()));
#pragma once
#include <cstddef>
#include <cstdint>

namespace dsc {

class IStorage {
public:
    virtual ~IStorage() = default;

    // 只读指针（始终有效）
    virtual const uint8_t *readOnlyMemPtr() const = 0;

    // 可写指针（不可写返回 nullptr，如 ROM mmap 模式）
    virtual uint8_t *writableMemPtr() { return nullptr; }

    // 紧凑存储的字节数
    virtual size_t memSize() const = 0;
};

} // namespace dsc
