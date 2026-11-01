// WallClock — 仿真时间对应的公历日历
//
// 基准取宿主当前时间（init/reset 时重新采样），之后每拍前进 ns_per_tick 纳秒。
// 输出年月日、时分秒和星期，便于电路里做日历/闹钟一类的逻辑。
#pragma once
#include "dcs/Component.h"

namespace dsc {

class WallClock : public SequentialComponent {
public:
    // ns_per_tick: 虚拟模式下每拍推进多少纳秒（1~1e9）
    // base_epoch: 非空时用这个 Unix 秒作为基准，空串表示用宿主当前时间
    // sync_host: true 时每个上升沿直接取宿主当前时间，忽略 ns_per_tick 累加
    WallClock(const std::string &name, int ns_per_tick, const std::string &base_epoch, bool sync_host);

    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

private:
    int _ns_per_tick;
    std::string _base_epoch;
    bool _sync_host;
};

} // namespace dsc
