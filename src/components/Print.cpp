// Print — 仿真监控元件实现
#include "dcs/components/Print.h"
#include <cstdio>
#include <format>
#include <stdexcept>

namespace dsc {

// 供 JIT 代码调用的打印函数（C 链接，避免 name mangling）
extern "C" void dcs_print(const char *name, const uint8_t *data, int bytes) {
    std::printf("[%s] ", name);
    for (int i = bytes - 1; i >= 0; i--)
        std::printf("%02X", data[i]);
    std::printf("\n");
    std::fflush(stdout);
}

Print::Print(const std::string &name, int bit_width) : SequentialComponent(name, "print"), _bit_width(bit_width) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    addInput("in", bit_width);
    addInput("clk", 1);
}

std::string Print::genStructDef() const {
    return std::format(R"(typedef struct {{
    uint8_t prev_clk;
}} {};)",
                       stateTypeName());
}

std::string Print::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string Print::genInitCode() const {
    return std::format("    {}.prev_clk = 0;", stateVarName());
}

// 修正初始化
std::string Print::genFuncDef_seq() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int clk_nid = inputs()[1]->netId();
    auto st = stateVarName();
    int bytes = byte_count(_bit_width);

    std::string clk_read = clk_nid >= 0 ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "    bool _clk = false;";

    return std::format(R"(extern void dcs_print(const char*, const uint8_t*, int);
static void {}() {{
    {}
    {}
    bool _rising = (_clk && !{}.prev_clk);
    if (_rising) {{
        dcs_print("{}", (const uint8_t*)&_val, {});
    }}
    {}.prev_clk = _clk;
}})",
                       funcName_seq(), gen_read_wire(in_nid, _bit_width, in_nw, "_val"), clk_read, st, name(), bytes,
                       st);
}

std::vector<Component::JitSymbol> Print::extraJitSymbols() const {
    return {{"dcs_print", reinterpret_cast<void *>(&dcs_print)}};
}

std::unique_ptr<Component> Print::clone(const std::string &n) const {
    return std::make_unique<Print>(n, _bit_width);
}

} // namespace dsc
