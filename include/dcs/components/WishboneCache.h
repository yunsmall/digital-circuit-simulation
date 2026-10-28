// WishboneCache — 直接映射的数据缓存，串接在 CPU 与存储器之间
//
// 上游是 Wishbone 从设备（面对 CPU），下游是 Wishbone 主设备（面对存储器）。
// 行大小固定 4 个字（16 字节），行数可配；读未命中整行填充，写采用写直通，
// 同时更新命中的行，避免后续读到脏数据。
//
// 参数:
//   lines: 缓存行数（必须是 2 的幂）
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
    std::vector<JitSymbol> extraJitSymbols() const override;

    std::string genStructDef() const override { return ""; }
    std::string genStateDecl() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override { return ""; }
    std::string genResetCode() const override { return genInitCode(); }
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 统计接口，便于测试和性能分析
    uint64_t hits() const { return _state.hits; }
    uint64_t misses() const { return _state.misses; }

private:
    static constexpr int LINE_WORDS = 4; // 一行 4 个字

    struct CacheLine {
        bool valid;
        uint32_t tag;             // 行地址
        uint32_t data[LINE_WORDS];
    };

    struct CacheState {
        int num_lines;
        CacheLine line[256];      // 上限 256 行，实际只用 num_lines
        // 上游事务
        uint32_t up_adr, up_dat;
        bool up_we, up_stb, up_cyc;
        uint32_t up_latched;
        bool up_ack;
        bool prev_up_stb;
        // 下游事务
        uint32_t dn_adr, dn_dat;
        bool dn_we, dn_stb, dn_cyc;
        bool prev_dn_stb;
        // 填充状态
        int fill_state;           // 0=空闲, 1=发读, 2=等 ack, 3=收数据
        uint32_t fill_addr;
        uint32_t fill_line;
        int fill_word;
        uint32_t pending_adr;     // 未命中时记住原始请求
        bool pending_we;
        uint32_t pending_wdata;
        uint64_t hits, misses;
    };

    CacheState _state;
    int _lines = 16;
    bool _prepared = false;

    static void cacheTick(void *state_ptr, const uint8_t *inputs, uint8_t *outputs);
    static void cacheReset(void *state_ptr, int lines);
};

} // namespace dsc
