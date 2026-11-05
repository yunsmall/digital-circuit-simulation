#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <ostream>

namespace dsc {

// 按 16 字节一行输出 [addr, addr+len) 的十六进制和可打印字符。
// 宿主侧调试工具（dcs/dcs_soc 的 --dump-mem）共用，避免两份逐行相同的格式代码。
// 地址按容量别名回绕（和 ELF/--load 一致），addr 超过 cap 只是绕回窗口内，不会越界
inline void dumpMemoryHex(std::ostream &os, const uint8_t *base, size_t cap, size_t addr, size_t len) {
    for (size_t row = 0; row < len; row += 16) {
        const size_t off = (addr + row) & (cap - 1);
        os << std::format("0x{:08X}  ", addr + row);
        for (size_t k = 0; k < 16; ++k) {
            if (row + k < len)
                os << std::format("{:02X} ", base[(off + k) & (cap - 1)]);
            else
                os << "   ";
        }
        os << " ";
        for (size_t k = 0; k < 16 && row + k < len; ++k) {
            const char ch = (char) base[(off + k) & (cap - 1)];
            os << (ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        os << "\n";
    }
}

} // namespace dsc
