// IStorage — 存储接口
//
// 拥有内部紧凑内存的元件（Memory、WishboneMemory、ROM、FIFO、MultiPortRAM、
// VirtioBlkMem）实现此接口。
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
    //
    // 约定：
    //   - memSize() 表示元件的逻辑容量，只由参数决定，不要求存储已经分配；
    //     例外是 ROM 文件模式，容量要读了文件才知道，未 prepare 时返回 0；
    //   - 元件在 prepare() 成功之前处于未就绪状态（见 Component.h 的生命周期
    //     注释），两个指针函数一律返回 nullptr。框架侧必须先 addComponent 再
    //     访问，不要指望访问器替你补做分配；
    //   - readOnlyMemPtr() 是 const 方法，不分配、不改状态；
    //   - writableMemPtr() 返回 nullptr 表示该存储不可写（如 ROM 的 mmap 模式）。
    // 存储的分配全部发生在 prepare() 里，访问器只做转发。
    // 只读指针（prepare 成功后有效）
    virtual const uint8_t *readOnlyMemPtr() const = 0;

    // 可写指针（未 prepare 或不可写返回 nullptr）
    virtual uint8_t *writableMemPtr() { return nullptr; }

    // 逻辑容量（字节数），按参数计算，不依赖存储是否已分配
    virtual size_t memSize() const = 0;
};

} // namespace dsc
