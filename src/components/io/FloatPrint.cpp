// FloatPrint — 浮点打印元件实现
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/FloatPrint.h"
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

namespace dsc {

// 精度由 owner 自己决定，JIT 只把原始位模式送过来；这样不同精度的实例
// 共用同一个宿主符号，不需要按精度拆两个入口
extern "C" void dcs_fprint(void *owner, const uint8_t *data) {
    static_cast<FloatPrint *>(owner)->emitValue(data);
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

// circuit_init/circuit_reset 都先把全部线网清零，再执行元件的初始化代码，
// 所以 clk 的上一拍电平就是 0，prev_clk 从 0 起不会漏掉/伪造上升沿
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

    const auto owner = (unsigned long long) reinterpret_cast<uintptr_t>(this);

    return std::format(R"(extern void dcs_fprint(void*, const uint8_t*);
static void {0}() {{
    {1}
    {2}
    bool _rising = (_clk && !{3}.prev_clk);
    if (_rising) {{
        uint8_t _buf[{4}];
        dcs_memcpy(_buf, &_val, {4});
        dcs_fprint((void *)0x{5:X}ULL, _buf);
    }}
    {3}.prev_clk = _clk;
}})",
                       funcName_seq(), gen_read_wire(in_nid, _precision, in_nw, "_val"), clk_read, st, bytes, owner);
}

void FloatPrint::setSink(const TextSink &sink, void *ctx) {
    _sink = sink;
    _sink_ctx = ctx;
}

void FloatPrint::emitValue(const uint8_t *data) {
    double v;
    if (_precision == 32) {
        float f;
        std::memcpy(&f, data, 4);
        v = (double) f;
    } else {
        std::memcpy(&v, data, 8);
    }
    const std::string line = std::format("[{}] {:g}\n", name(), v);
    if (_sink.on_text) {
        _sink.on_text(_sink_ctx, this, line.data(), (int) line.size());
        return;
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

std::vector<Component::JitSymbol> FloatPrint::extraJitSymbols() const {
    return {{"dcs_fprint", reinterpret_cast<void *>(&dcs_fprint)}};
}

std::unique_ptr<Component> FloatPrint::clone(const std::string &n) const {
    return std::make_unique<FloatPrint>(n, _precision);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"fprint", {{"precision", "32", {"32", "64"}}}, {"in", "clk"}, {}, dsc::ComponentCategory::Float, "浮点打印，clk上升沿打印浮点值"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::FloatPrint>(n, dsc::paramInt(p, "precision", 32));
                       });
} // namespace
