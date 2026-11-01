// WishboneDcache — 串在 CPU 与互连之间的写直通数据缓存（带总线监听）
//
// 上游是 Wishbone 从端口（面对 CPU），下游是 Wishbone 主端口（面对互连），
// 另有 sn_* 监听端口接互连的 RAM 从设备侧总线：其它主设备（virtio DMA 等）
// 写 RAM 时把对应行失效，CPU 下次读会重新去存储器取。
//
// 时序：
//   - 命中读由组合阶段直接应答，CPU 在下一拍就能拿到数据
//   - 未命中/写直通走主端口，和"不接缓存直接挂总线"差不多，只多一拍总线往返
//   - 非 cacheable 段（MMIO）原样旁路，不占缓存行
//
// 生成代码依赖互连的 seq 在本元件之前执行（同一个 tick 里能看到主端口的 ack），
// 所以 JSON 里要放在 ic 之后、ram 之前。
//
// 参数:
//   lines: 缓存行数（2 的幂，默认 1024，即 16 KB）
//   base/size: 可缓存物理地址段，段外访问一律旁路
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class WishboneDcache : public Component {
public:
    explicit WishboneDcache(const std::string &name, int lines = 1024, uint32_t base = 0x80000000u,
                            uint32_t size = 0x10000000u);
    ~WishboneDcache() override;

    bool prepare(std::string &error) override;
    [[nodiscard]] std::vector<JitSymbol> extraJitSymbols() const override {
        return {};
    }

    [[nodiscard]] std::string genStateDecl() const override;
    [[nodiscard]] std::string genInitCode() const override;
    [[nodiscard]] std::string genResetCode() const override {
        return genInitCode();
    }
    [[nodiscard]] bool hasSequential() const override {
        return true;
    }
    [[nodiscard]] std::string genFuncDef_comb() const override;
    [[nodiscard]] std::string genFuncDef_seq() const override;

    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    // 统计（[0]=命中 [1]=未命中 [2]=总访问），用于性能分析
    uint64_t hits() const {
        return _stats[0];
    }
    uint64_t misses() const {
        return _stats[1];
    }
    uint64_t accesses() const {
        return _stats[2];
    }

    // 组合阶段只读 CPU 侧和主端口应答，监听引脚只由时序阶段使用；
    // 环路检测如果把它们算进来，会把"监听口直接挂在主端口上"的合法接法误报成环
    [[nodiscard]] std::vector<int> getCombinationalDeps(int out_idx) const override;

    int lines() const {
        return _lines;
    }

private:
    int _lines;
    uint32_t _base;
    uint32_t _size;
    bool _prepared = false;
    // 计数器放在 C++ 侧，JIT 通过 genStateDecl 暴露的指针累加
    uint64_t _stats[3] = {0, 0, 0};
};

} // namespace dsc
