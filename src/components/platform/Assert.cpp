#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/Assert.h"

#include <cstdio>
#include <format>

namespace dsc {

struct Assert::State {
    uint64_t fails = 0;
};

extern "C" void dcs_assert_fail(void *owner, unsigned long long count) {
    static_cast<Assert *>(owner)->onFailFromJit((uint64_t) count);
}

Assert::Assert(const std::string &name, const std::string &message, int max_reports) :
    SequentialComponent(name, "assert"),
    _state(std::make_unique<State>()),
    _message(message),
    _max_reports(max_reports) {
    setParam("message", message);
    setParam("max_reports", std::to_string(max_reports));
    addInput("clk", 1, PinEdge::Rising);
    addInput("cond", 1);
}

Assert::~Assert() = default;

uint64_t Assert::failCount() const {
    return _state->fails;
}

void Assert::onFailFromJit(uint64_t count) {
    _state->fails = count;
    if (_max_reports > 0 && count <= (uint64_t) _max_reports) {
        std::fprintf(stderr, "[assert] %s 失败（第 %llu 次）%s\n", name().c_str(), (unsigned long long) count,
                     _message.empty() ? "" : _message.c_str());
        std::fflush(stderr);
    }
}

std::string Assert::genStructDef() const {
    return std::format("typedef struct {{\n    uint64_t fails;\n}} {};", stateTypeName());
}

std::string Assert::genStateDecl() const {
    return std::format("static {} {};\nstatic void *_assert_owner_{};", stateTypeName(), stateVarName(), _id);
}

std::string Assert::genInitCode() const {
    return std::format("    {}.fails = 0; _assert_owner_{} = (void *)0x{:X}ULL;", stateVarName(), _id,
                       (unsigned long long) reinterpret_cast<uintptr_t>(this));
}

std::string Assert::genFuncDef_seq() const {
    int cond_nid = inputs()[1]->netId();
    int cond_nw = cond_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    auto st = stateVarName();

    std::string c;
    c += "extern void dcs_assert_fail(void *, unsigned long long);\n";
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    " + gen_read_wire(cond_nid, 1, cond_nw, "_cond") + "\n";
    c += std::format("    if ({} && !(_cond & 1)) {{\n", genGetWireEdge(0, PinEdge::Rising));
    c += std::format("        {}.fails++;\n", st);
    // 每次都回调，宿主才能读到准确次数；打印次数由回调按 max_reports 限制
    c += std::format("        dcs_assert_fail(_assert_owner_{}, {}.fails);\n", _id, st);
    c += "    }\n";
    c += "}\n";
    return c;
}

std::vector<Component::JitSymbol> Assert::extraJitSymbols() const {
    return {{"dcs_assert_fail", reinterpret_cast<void *>(&dcs_assert_fail)}};
}

std::unique_ptr<Component> Assert::clone(const std::string &n) const {
    return std::make_unique<Assert>(n, _message, _max_reports);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"assert", {{"message", ""}, {"max_reports", "1"}}, {"clk", "cond"}, {}, dsc::ComponentCategory::Misc,
     "仿真断言：clk 上升沿 cond=0 时记一次失败并打印（最多 max_reports 次）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Assert>(n, dsc::paramStr(p, "message", ""),
                                                                dsc::paramInt(p, "max_reports", 1));
                       });
} // namespace
