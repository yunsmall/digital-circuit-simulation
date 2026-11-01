// Assert — 仿真期条件检查器
//
// 时钟上升沿检查 cond，为 0 就记一次失败。只打印前 max_reports 次，
// 避免一个错误的时钟接法把日志刷爆；次数可以随时通过 failCount() 读。
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "dcs/Component.h"

namespace dsc {

class Assert : public SequentialComponent {
public:
    // message: 失败时打印的说明
    // max_reports: 最多打印几次（0 = 不打印，只计数）
    Assert(const std::string &name, const std::string &message, int max_reports);
    ~Assert() override;

    uint64_t failCount() const;

    std::string genStructDef() const override;
    std::string genStateDecl() const override;
    std::string genInitCode() const override;
    std::string genFuncDef_seq() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    void onFailFromJit(uint64_t count);

private:
    struct State;
    std::unique_ptr<State> _state;
    std::string _message;
    int _max_reports;
};

} // namespace dsc
