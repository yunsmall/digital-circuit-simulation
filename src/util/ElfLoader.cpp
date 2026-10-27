#include "dcs/util/ElfLoader.h"

#include <cstring>

#include <elfio/elfio.hpp>

namespace dsc {

namespace {
// 段是否需要在复位后出现在地址空间里
bool isLoadable(const ELFIO::section *s) {
    return (s->get_flags() & ELFIO::SHF_ALLOC) != 0;
}
} // namespace

ElfImage loadElf32(const std::string &path) {
    ElfImage img;
    ELFIO::elfio reader;
    if (!reader.load(path)) {
        img.error = "无法解析 ELF 文件: " + path;
        return img;
    }
    if (reader.get_class() != ELFIO::ELFCLASS32 || reader.get_encoding() != ELFIO::ELFDATA2LSB) {
        img.error = "只支持 ELF32 小端";
        return img;
    }

    // 先定出映像范围：CPU 的地址空间从 0 开始，映像放不下就没法直接灌进存储器
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto &up : reader.sections) {
        const ELFIO::section *s = up.get();
        if (!isLoadable(s) || s->get_size() == 0)
            continue;
        lo = std::min(lo, static_cast<uint32_t>(s->get_address()));
        hi = std::max(hi, static_cast<uint32_t>(s->get_address() + s->get_size()));
    }
    if (lo == UINT32_MAX) {
        img.error = "ELF 里没有可装载的段";
        return img;
    }
    lo &= ~0xFFFu; // 对齐到页，避免 base 落在段中间

    img.base = lo;
    img.entry = static_cast<uint32_t>(reader.get_entry());
    img.data.assign(hi - lo, 0);

    for (const auto &up : reader.sections) {
        const ELFIO::section *s = up.get();
        if (!isLoadable(s) || s->get_size() == 0)
            continue;
        // SHT_NOBITS（.bss）在文件里没有内容，内存映像已经清 0
        if (s->get_type() == ELFIO::SHT_NOBITS)
            continue;
        const uint32_t addr = static_cast<uint32_t>(s->get_address());
        const size_t n = static_cast<size_t>(s->get_size());
        if (addr < lo || addr - lo + n > img.data.size()) {
            img.error = "段超出映像范围: " + s->get_name();
            return img;
        }
        std::memcpy(img.data.data() + (addr - lo), s->get_data(), n);
    }

    // riscv-tests 的通过/失败靠写 tohost 判定，顺手把符号地址找出来
    for (const auto &up : reader.sections) {
        ELFIO::section *s = up.get();
        if (s->get_type() != ELFIO::SHT_SYMTAB)
            continue;
        ELFIO::symbol_section_accessor syms(reader, s);
        for (unsigned i = 0; i < syms.get_symbols_num(); ++i) {
            std::string name;
            ELFIO::Elf64_Addr value = 0;
            ELFIO::Elf_Xword size = 0;
            unsigned char bind = 0, type = 0, other = 0;
            ELFIO::Elf_Half shndx = 0;
            if (!syms.get_symbol(i, name, value, size, bind, type, shndx, other))
                continue;
            if (name == "tohost")
                img.tohost = static_cast<uint32_t>(value);
        }
    }

    img.ok = true;
    return img;
}

} // namespace dsc
