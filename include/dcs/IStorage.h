// IStorage — 存储接口
//
// 拥有内部紧凑内存的元件（Memory、ROM、FIFO、MultiPortRAM）实现此接口。
//
// 下面这些裸指针函数**只允许仿真框架调用**：ELF 装载、测试布置数据、
// --dump-mem 这类扮演"外部世界"的工具。任何元件都禁止调用它们。
//
// 理由：真实硬件上元件之间不存在"拿别人内部存储指针"的接口。元件要读写
// 别人的存储，只能通过自己的端口发起总线事务（参考 DMA、VirtioBlk 的主端口）。
// 用这些指针就是仿真专用的后门，会让电路里跑的行为和真实硬件对不上。
//
// 用法示例（仅限框架侧代码）：
//   auto *s = dynamic_cast<IStorage*>(comp);
//   const uint8_t *raw = s->readOnlyMemPtr();
//   size_t n = s->memSize();
//   uint8_t *wp = s->writableMemPtr();
//   if (wp) std::memcpy(wp, my_data, std::min(sizeof(my_data), s->memSize()));
#pragma once
#include <cstddef>
#include <cstdint>

namespace dsc {

class IStorage {
public:
    virtual ~IStorage() = default;

    // 以下三个函数仅供仿真框架使用，元件禁止调用（原因见文件头）
    // 只读指针（始终有效）
    virtual const uint8_t *readOnlyMemPtr() const = 0;

    // 可写指针（不可写返回 nullptr，如 ROM mmap 模式）
    virtual uint8_t *writableMemPtr() { return nullptr; }

    // 紧凑存储的字节数
    virtual size_t memSize() const = 0;
};

} // namespace dsc
