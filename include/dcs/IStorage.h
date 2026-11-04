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
    //
    // 约定：
    //   - memSize() 表示元件的逻辑容量，只由参数决定，不要求存储已经分配；
    //   - readOnlyMemPtr() 是 const 方法，不分配、不改状态：元件还没 prepare()
    //     且没访问过可写指针时可以是 nullptr；
    //   - writableMemPtr() 是非 const 方法，内存型存储在第一次调用时才分配
    //     （prepare() 正常加入电路时已经分配好）。宿主预加载数据用它即可。
    // 只读指针（prepare 或首次 writableMemPtr 后有效）
    virtual const uint8_t *readOnlyMemPtr() const = 0;

    // 可写指针（不可写返回 nullptr，如 ROM mmap 模式）；首次调用可能分配存储
    virtual uint8_t *writableMemPtr() { return nullptr; }

    // 逻辑容量（字节数），按参数计算，不依赖存储是否已分配
    virtual size_t memSize() const = 0;
};

} // namespace dsc
