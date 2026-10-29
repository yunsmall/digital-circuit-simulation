// ELF 映像加载：把可执行文件里需要装载的段摊平到一块内存，供 CPU 类元件取指。
// 解析用 elfio（header-only），不依赖仿真后端，LLVM/TCC 两种配置都能用。
// 32 位与 64 位各一个入口：两边的地址空间和符号宽度不同，混在一个函数里
// 反而要到处判位宽，容易在 64 位映像上悄悄截断地址。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsc {

struct ElfImage {
    std::vector<uint8_t> data; // 从 base 开始的平铺内存映像，未装载区域为 0
    uint64_t base = 0;         // 映像起始地址（所有装载段地址的最小值，向下 4KB 对齐）
    uint64_t entry = 0;        // 入口地址（绝对地址）
    uint64_t tohost = 0;       // tohost 符号的绝对地址，riscv-tests 靠它报告结果；找不到为 0
    bool ok = false;
    std::string error;
};

// 只接受对应位宽的小端 ELF；失败时 ok=false 并在 error 里说明原因
ElfImage loadElf32(const std::string &path);
ElfImage loadElf64(const std::string &path);

} // namespace dsc
