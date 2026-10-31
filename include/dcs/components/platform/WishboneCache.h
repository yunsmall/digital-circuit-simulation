// WishboneCache — 直接映射的数据缓存，串接在 CPU 与存储器之间
//
// 上游是 Wishbone 从设备（面对 CPU），下游是 Wishbone 主设备（面对存储器）。
// 行大小固定 4 个字（16 字节），行数可配；读未命中整行填充，写采用写直通，
// 同时更新命中的行，避免后续读到脏数据。
//
// 参数:
//   lines: 缓存行数（按 lines 取模索引，不要求是 2 的幂）
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class WishboneCache : public SequentialComponent {
public:
    explicit WishboneCache(const std::string &name, int lines = 16);
    ~WishboneCache() override;

    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override { return {}; }

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 统计接口，便于测试和性能分析。计数器放在 C++ 侧（[0]=命中的、[1]=未命中），
    // JIT 代码通过 genStateDecl 暴露的指针递增，这样外部仍能随时查询。
    uint64_t hits() const { return _stats[0]; }
    uint64_t misses() const { return _stats[1]; }

private:
    static constexpr int LINE_WORDS = 4; // 一行 4 个字

    uint64_t _stats[2] = {0, 0};
    int _lines = 16;
    bool _prepared = false;
};

} // namespace dsc
