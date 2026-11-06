#include "dcs/ComponentFactory.h"
#include "dcs/components/sequential/Debounce.h"

#include <format>
#include <stdexcept>

namespace dsc {

Debounce::Debounce(const std::string &name, int bit_width, int stable_ticks) :
    SequentialComponent(name, "debounce"), _bit_width(bit_width), _stable_ticks(stable_ticks) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    if (stable_ticks < 1 || stable_ticks > 65535)
        throw std::invalid_argument("stable_ticks 必须在1-65535之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("stable_ticks", std::to_string(stable_ticks));
    addInput("clk", 1, PinEdge::Rising);
    addInput("in", bit_width);
    addOutput("out", bit_width, false, true);
}

std::string Debounce::genStructDef() const {
    return std::format("typedef struct {{\n    uint64_t stable;\n    uint32_t cnt;\n}} {};", stateTypeName());
}

std::string Debounce::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string Debounce::genInitCode() const {
    auto st = stateVarName();
    return std::format("    {}.stable = 0; {}.cnt = 0;", st, st);
}

std::string Debounce::genFuncDef_seq() const {
    int in_nid = inputs()[1]->netId();
    int in_nw = in_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    auto st = stateVarName();

    std::string c;
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    " + gen_read_wire(in_nid, _bit_width, in_nw, "_in") + "\n";
    c += "    const uint64_t _v = (uint64_t) _in & " + gen_mask(_bit_width) + ";\n";
    c += "    if (" + genGetWireEdge(0, PinEdge::Rising) + ") {\n";
    // 回到稳定值就清零：抖动期间任何一次回弹都会让计数从头开始
    c += std::format("        if (_v == {}.stable) {}.cnt = 0;\n", st, st);
    c += std::format("        else if (++{0}.cnt >= {1}u) {{ {0}.stable = _v; {0}.cnt = 0; }}\n", st, _stable_ticks);
    c += "    }\n";
    c += std::format("    uint64_t _out = {}.stable;\n", st);
    c += "    " + genOutputWrite(0, "_out", _bit_width) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Debounce::clone(const std::string &n) const {
    return std::make_unique<Debounce>(n, _bit_width, _stable_ticks);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"debounce", {{"bit_width", "1"}, {"stable_ticks", "8"}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential,
     "按键去抖：输入稳定 stable_ticks 个时钟沿后才更新输出"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Debounce>(n, dsc::paramInt(p, "bit_width", 1),
                                                                  dsc::paramInt(p, "stable_ticks", 8));
                       });
} // namespace
