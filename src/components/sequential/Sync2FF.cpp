#include "dcs/ComponentFactory.h"
#include "dcs/components/sequential/Sync2FF.h"

#include <format>
#include <stdexcept>

namespace dsc {

Sync2FF::Sync2FF(const std::string &name, int bit_width) :
    SequentialComponent(name, "sync2ff"), _bit_width(bit_width) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    addInput("clk", 1);
    addInput("d", bit_width);
    addOutput("q", bit_width, false, true);
}

std::string Sync2FF::genStructDef() const {
    return std::format("typedef struct {{\n    uint64_t s1, s2;\n    uint8_t prev_clk;\n}} {};", stateTypeName());
}

std::string Sync2FF::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string Sync2FF::genInitCode() const {
    auto st = stateVarName();
    return std::format("    {}.s1 = 0; {}.s2 = 0; {}.prev_clk = 0;", st, st, st);
}

std::string Sync2FF::genFuncDef_seq() const {
    int clk_nid = inputs()[0]->netId();
    int d_nid = inputs()[1]->netId();
    int d_nw = d_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    auto st = stateVarName();

    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    c += clk_nid >= 0 ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);\n", clk_nid)
                      : std::string("    bool _clk = false;\n");
    c += "    " + gen_read_wire(d_nid, _bit_width, d_nw, "_d") + "\n";
    // 第一级采异步输入，第二级再采第一级：第二级输出对源时钟域最多晚两拍
    c += std::format("    if (_clk && !{}.prev_clk) {{ {}.s2 = {}.s1; {}.s1 = (uint64_t) _d & {}; }}\n", st, st, st, st,
                     gen_mask(_bit_width));
    c += std::format("    {}.prev_clk = _clk;\n", st);
    c += std::format("    uint64_t _q = (uint64_t) {}.s2;\n", st);
    c += "    " + genOutputWrite(0, "_q", _bit_width) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Sync2FF::clone(const std::string &n) const {
    return std::make_unique<Sync2FF>(n, _bit_width);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"sync2ff", {{"bit_width", "1"}}, {"clk", "d"}, {"q"}, dsc::ComponentCategory::Sequential,
     "跨时钟域两级同步器，q 比 d 晚两个时钟沿"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Sync2FF>(n, dsc::paramInt(p, "bit_width", 1));
                       });
} // namespace
