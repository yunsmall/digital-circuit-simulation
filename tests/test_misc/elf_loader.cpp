// ELF 装载的边界：外部文件里的段地址/长度必须做无溢出检查
#include <gtest/gtest.h>

#include <chrono>
#include <dcs/util/ElfLoader.h>
#include <elfio/elfio.hpp>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

namespace {

// 生成一个 ELF64：两个可装载段，地址/长度由测试指定
std::string makeElf(uint64_t good_addr, size_t good_size, uint64_t bad_addr, size_t bad_size) {
    ELFIO::elfio w;
    w.create(ELFIO::ELFCLASS64, ELFIO::ELFDATA2LSB);
    w.set_type(ELFIO::ET_EXEC);
    w.set_machine(ELFIO::EM_RISCV);
    w.set_entry(good_addr);
    auto add = [&](const char *name, uint64_t addr, size_t size) {
        ELFIO::section *s = w.sections.add(name);
        s->set_type(ELFIO::SHT_PROGBITS);
        s->set_flags(ELFIO::SHF_ALLOC);
        s->set_addr_align(4);
        s->set_address(addr);
        std::string buf(size, '\0');
        s->set_data(buf.data(), (ELFIO::Elf_Word) size);
    };
    add(".text", good_addr, good_size);
    add(".evil", bad_addr, bad_size);

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / std::format("dcs-elf-{}.elf", stamp);
    EXPECT_TRUE(w.save(path.string()));
    return path.string();
}

} // namespace

// 段地址接近 UINT64_MAX：addr+size 在 uint64 上回绕，不能让 hi 变小后通过跨度检查
TEST(ElfLoader, RejectsAddressOverflow) {
    const std::string path = makeElf(0x1000, 0x100, 0xFFFFFFFFFFFFF000ULL, 0x2000);
    auto img = dsc::loadElf64(path);
    EXPECT_FALSE(img.ok);
    EXPECT_NE(img.error.find("溢出"), std::string::npos) << img.error;
    std::filesystem::remove(path);
}

TEST(ElfLoader, LoadsNormalImage) {
    const std::string path = makeElf(0x1000, 0x100, 0x2000, 0x40);
    auto img = dsc::loadElf64(path);
    ASSERT_TRUE(img.ok) << img.error;
    EXPECT_TRUE(img.is64);
    EXPECT_EQ(img.base, 0x1000u);
    EXPECT_EQ(img.data.size(), 0x1040u);
    std::filesystem::remove(path);
}
