// FloatPrint — 浮点打印元件实现
#include "dcs/components/FloatPrint.h"
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

namespace dsc {

extern "C" void dcs_fprint32(const char *name, const uint8_t *data) {
    float v;
    std::memcpy(&v, data, 4);
    std::printf("[%s] %g\n", name, (double) v);
    std::fflush(stdout);
}

extern "C" void dcs_fprint64(const char *name, const uint8_t *data) {
    double v;
    std::memcpy(&v, data, 8);
    std::printf("[%s] %g\n", name, v);
    std::fflush(stdout);
}

FloatPrint::FloatPrint(const std::string &name, int precision) :
    SequentialComponent(name, "fprint"), _precision(precision) {
    if (precision != 32 && precision != 64)
        throw std::invalid_argument("精度必须是 32 或 64");
    setParam("precision", std::to_string(precision));
    addInput("in", precision);
    addInput("clk", 1);
}

std::string FloatPrint::genStructDef() const {
    return std::format(R"(typedef struct {{ uint8_t prev_clk; }} {};)", stateTypeName());
}

std::string FloatPrint::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string FloatPrint::genInitCode() const {
    return std::format("    {}.prev_clk = 0;", stateVarName());
}

std::string FloatPrint::genFuncDef_seq() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int clk_nid = inputs()[1]->netId();
    auto st = stateVarName();
    int bytes = _precision / 8;

    std::string clk_read = clk_nid >= 0 ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "    bool _clk = false;";

    const char *dcs_func = (_precision == 32) ? "dcs_fprint32" : "dcs_fprint64";

    return std::format(R"(extern void {0}(const char*, const uint8_t*);
static void {1}() {{
    {2}
    {3}
    bool _rising = (_clk && !{4}.prev_clk);
    if (_rising) {{
        uint8_t _buf[{5}];
        dcs_memcpy(_buf, &_val, {5});
        {0}("{6}", _buf);
    }}
    {4}.prev_clk = _clk;
}})",
                       dcs_func, funcName_seq(), gen_read_wire(in_nid, _precision, in_nw, "_val"), clk_read, st, bytes,
                       name());
}

std::vector<Component::JitSymbol> FloatPrint::extraJitSymbols() const {
    if (_precision == 32)
        return {{"dcs_fprint32", reinterpret_cast<void *>(&dcs_fprint32)}};
    else
        return {{"dcs_fprint64", reinterpret_cast<void *>(&dcs_fprint64)}};
}

std::unique_ptr<Component> FloatPrint::clone(const std::string &n) const {
    return std::make_unique<FloatPrint>(n, _precision);
}

} // namespace dsc
