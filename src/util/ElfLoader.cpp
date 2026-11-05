#include "dcs/util/ElfLoader.h"

#include <cstring>
#include <fstream>

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
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const auto &up : reader.sections) {
        const ELFIO::section *s = up.get();
        if (!isLoadable(s) || s->get_size() == 0)
            continue;
        // 地址和长度都来自外部文件：先证明 addr+size 不溢出，再参与跨度计算，
        // 否则构造过的 ELF 能让 hi 回绕、跨度检查形同虚设
        const uint64_t seg_addr = static_cast<uint64_t>(s->get_address());
        const uint64_t seg_size = static_cast<uint64_t>(s->get_size());
        if (seg_addr > UINT64_MAX - seg_size) {
            img.error = "段地址溢出: " + s->get_name();
            return img;
        }
        lo = std::min(lo, seg_addr);
        hi = std::max(hi, seg_addr + seg_size);
    }
    if (lo == UINT64_MAX) {
        img.error = "ELF 里没有可装载的段";
        return img;
    }
    lo &= ~0xFFFu; // 对齐到页，避免 base 落在段中间

    // 稀疏布局可能把映像拉得极大（.text 在低地址、另有一个段在高地址）：
    // 铺平整个区间等于按最远地址分配内存，一个构造过的 ELF 就能把内存吃光
    constexpr uint64_t MAX_IMAGE_SPAN = 256ull << 20;
    if (hi < lo || hi - lo > MAX_IMAGE_SPAN) {
        img.error = "映像跨度异常: 段分布过于稀疏或地址溢出";
        return img;
    }

    img.base = lo;
    img.entry = static_cast<uint64_t>(reader.get_entry());
    img.data.assign(hi - lo, 0);

    for (const auto &up : reader.sections) {
        const ELFIO::section *s = up.get();
        if (!isLoadable(s) || s->get_size() == 0)
            continue;
        // SHT_NOBITS（.bss）在文件里没有内容，内存映像已经清 0
        if (s->get_type() == ELFIO::SHT_NOBITS)
            continue;
        const uint64_t addr = static_cast<uint64_t>(s->get_address());
        const size_t n = static_cast<size_t>(s->get_size());
        // 边界检查用减法，避免 addr - lo + n 在 uint64 上回绕后通过检查
        if (addr < lo) {
            img.error = "段超出映像范围: " + s->get_name();
            return img;
        }
        const uint64_t off = addr - lo;
        if (off > (uint64_t) img.data.size() || n > img.data.size() - (size_t) off) {
            img.error = "段超出映像范围: " + s->get_name();
            return img;
        }
        const char *seg_data = s->get_data();
        if (n && !seg_data) {
            img.error = "段数据为空: " + s->get_name();
            return img;
        }
        std::memcpy(img.data.data() + (size_t) off, seg_data, n);
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
                img.tohost = value;
        }
    }

    img.ok = true;
    return img;
}

ElfImage loadElf64(const std::string &path) {
    ElfImage img;
    ELFIO::elfio reader;
    if (!reader.load(path)) {
        img.error = "无法解析 ELF 文件: " + path;
        return img;
    }
    img.is64 = true;
    if (reader.get_class() != ELFIO::ELFCLASS64 || reader.get_encoding() != ELFIO::ELFDATA2LSB) {
        img.error = "只支持 ELF64 小端";
        return img;
    }

    // 先定出映像范围：CPU 的地址空间从 0 开始，映像放不下就没法直接灌进存储器
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const auto &up : reader.sections) {
        const ELFIO::section *s = up.get();
        if (!isLoadable(s) || s->get_size() == 0)
            continue;
        // 地址和长度都来自外部文件：先证明 addr+size 不溢出，再参与跨度计算，
        // 否则构造过的 ELF 能让 hi 回绕、跨度检查形同虚设
        const uint64_t seg_addr = static_cast<uint64_t>(s->get_address());
        const uint64_t seg_size = static_cast<uint64_t>(s->get_size());
        if (seg_addr > UINT64_MAX - seg_size) {
            img.error = "段地址溢出: " + s->get_name();
            return img;
        }
        lo = std::min(lo, seg_addr);
        hi = std::max(hi, seg_addr + seg_size);
    }
    if (lo == UINT64_MAX) {
        img.error = "ELF 里没有可装载的段";
        return img;
    }
    lo &= ~0xFFFu; // 对齐到页，避免 base 落在段中间

    // 稀疏布局可能把映像拉得极大（.text 在低地址、另有一个段在高地址）：
    // 铺平整个区间等于按最远地址分配内存，一个构造过的 ELF 就能把内存吃光
    constexpr uint64_t MAX_IMAGE_SPAN = 256ull << 20;
    if (hi < lo || hi - lo > MAX_IMAGE_SPAN) {
        img.error = "映像跨度异常: 段分布过于稀疏或地址溢出";
        return img;
    }

    img.base = lo;
    img.entry = static_cast<uint64_t>(reader.get_entry());
    img.data.assign(hi - lo, 0);

    for (const auto &up : reader.sections) {
        const ELFIO::section *s = up.get();
        if (!isLoadable(s) || s->get_size() == 0)
            continue;
        // SHT_NOBITS（.bss）在文件里没有内容，内存映像已经清 0
        if (s->get_type() == ELFIO::SHT_NOBITS)
            continue;
        const uint64_t addr = static_cast<uint64_t>(s->get_address());
        const size_t n = static_cast<size_t>(s->get_size());
        // 边界检查用减法，避免 addr - lo + n 在 uint64 上回绕后通过检查
        if (addr < lo) {
            img.error = "段超出映像范围: " + s->get_name();
            return img;
        }
        const uint64_t off = addr - lo;
        if (off > (uint64_t) img.data.size() || n > img.data.size() - (size_t) off) {
            img.error = "段超出映像范围: " + s->get_name();
            return img;
        }
        const char *seg_data = s->get_data();
        if (n && !seg_data) {
            img.error = "段数据为空: " + s->get_name();
            return img;
        }
        std::memcpy(img.data.data() + (size_t) off, seg_data, n);
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
                img.tohost = value;
        }
    }

    img.ok = true;
    return img;
}

} // namespace dsc

namespace dsc {
ElfImage loadElf(const std::string &path) {
    // EI_CLASS 在 ELF 头偏移 4：1=32 位，2=64 位。先读头再分派，
    // 免得调用方要自己知道镜像是哪个位宽
    std::ifstream f(path, std::ios::binary);
    unsigned char hdr[5] = {0};
    if (!f.read(reinterpret_cast<char *>(hdr), 5)
        || hdr[0] != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') {
        ElfImage bad;
        bad.error = "不是 ELF 文件";
        return bad;
    }
    return (hdr[4] == 2) ? loadElf64(path) : loadElf32(path);
}
} // namespace dsc