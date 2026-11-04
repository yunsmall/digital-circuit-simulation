// Print — 仿真监控元件实现
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Print.h"
#include <cstdio>
#include <format>
#include <stdexcept>

namespace dsc {

// 供 JIT 代码调用的打印入口（C 链接，避免 name mangling）。
// owner 是元件实例，回调按实例分发，不再有进程级全局 sink
extern "C" void dcs_print(void *owner, const uint8_t *data, int bytes) {
    static_cast<Print *>(owner)->emitValue(data, bytes);
}

extern "C" void dcs_print_char(void *owner, unsigned char c) {
    static_cast<Print *>(owner)->emitChar(c);
}

Print::Print(const std::string &name, int bit_width, bool ascii) :
    SequentialComponent(name, "print"), _bit_width(bit_width), _ascii(ascii) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("ascii", ascii ? "1" : "0");
    addInput("in", bit_width);
    addInput("clk", 1);
    // 门控输入。十六进制模式下 en 是电平门控（clk 上升沿且 en 为高才交）；
    // 字符模式下 en 是"有效字节"选通，按它的上升沿交（UartTerm 的 valid 只高一拍，
    // 且不一定和 clk 对齐）。不接时按恒使能处理，字符模式退回 clk 上升沿，
    // 这样老的电路 JSON（没有 en 连接）行为不变
    addInput("en", 1);
}

std::string Print::genStructDef() const {
    return std::format(R"(typedef struct {{
    uint8_t prev_clk;
    uint8_t prev_en;
}} {};)",
                       stateTypeName());
}

std::string Print::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

// circuit_init/circuit_reset 都先把全部线网清零，再执行元件的初始化代码，
// 所以 clk 的上一拍电平就是 0，prev_clk 从 0 起不会漏掉/伪造上升沿
std::string Print::genInitCode() const {
    return std::format("    {}.prev_clk = 0; {}.prev_en = 0;", stateVarName(), stateVarName());
}

// 修正初始化
std::string Print::genFuncDef_seq() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int clk_nid = inputs()[1]->netId();
    int en_nid = inputs()[2]->netId();
    auto st = stateVarName();
    int bytes = byte_count(_bit_width);

    std::string clk_read = clk_nid >= 0 ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "    bool _clk = false;";
    std::string en_read = en_nid >= 0
                              ? gen_read_wire(en_nid, 1, inputs()[2]->net()->bit_width(), "_en")
                              : "    bool _en = true;";

    // 字符模式下 en 是"有效字节"选通（UartTerm 的 valid 只高一拍，而且不一定和
    // clk 对齐），所以接了 en 时用它的上升沿触发，保证一个字节只打一次。
    // 没接 en 的老电路按恒使能处理，退回 clk 上升沿，否则只会输出第一拍
    const auto owner = (unsigned long long) reinterpret_cast<uintptr_t>(this);
    std::string emit;
    if (_ascii) {
        const char *cond = en_nid >= 0 ? "_en_rising" : "_rising";
        emit = std::format("    if ({}) dcs_print_char((void *)0x{:X}ULL, (unsigned char) (_val & 0xFF));", cond, owner);
    } else {
        emit = std::format("    if (_rising && _en) dcs_print((void *)0x{:X}ULL, (const uint8_t*)&_val, {});", owner, bytes);
    }

    return std::format(R"(extern void dcs_print(void*, const uint8_t*, int);
extern void dcs_print_char(void*, unsigned char);
static void {}() {{
    {}
    {}
    {}
    bool _rising = (_clk && !{}.prev_clk);
    bool _en_rising = (_en && !{}.prev_en);
{}
    {}.prev_clk = _clk;
    {}.prev_en = _en;
}})",
                       funcName_seq(), gen_read_wire(in_nid, _bit_width, in_nw, "_val"), clk_read, en_read, st, st, emit,
                       st, st);
}

std::vector<Component::JitSymbol> Print::extraJitSymbols() const {
    return {{"dcs_print", reinterpret_cast<void *>(&dcs_print)},
            {"dcs_print_char", reinterpret_cast<void *>(&dcs_print_char)}};
}

void Print::setSink(const TextSink &sink, void *ctx) {
    _sink = sink;
    _sink_ctx = ctx;
}

void Print::emitValue(const uint8_t *data, int bytes) {
    std::string line = std::format("[{}] ", name());
    for (int i = bytes - 1; i >= 0; i--)
        line += std::format("{:02X}", (unsigned) data[i]);
    line += '\n';
    if (_sink.on_text) {
        _sink.on_text(_sink_ctx, this, line.data(), (int) line.size());
        return;
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

// 字符模式下不能拼行：串口是一个字节一个字节到达的，攒到换行会把交互式输入
// 一直卡在缓冲区里。无 sink 时保留原来的行为，遇换行立刻刷出
void Print::emitChar(unsigned char c) {
    if (_sink.on_text) {
        const char ch = (char) c;
        _sink.on_text(_sink_ctx, this, &ch, 1);
        return;
    }
    std::fputc((int) c, stdout);
    if (c == '\n')
        std::fflush(stdout);
}

std::unique_ptr<Component> Print::clone(const std::string &n) const {
    return std::make_unique<Print>(n, _bit_width, _ascii);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"print", {{"bit_width", "8"}, {"ascii", "0", {"0", "1"}}}, {"in", "clk", "en"}, {}, dsc::ComponentCategory::Misc, "整型打印，clk上升沿打印当前值（en 为高才打印，ascii=1 时按字符输出；可按实例 setSink 接管输出）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Print>(n, dsc::paramInt(p, "bit_width", 8), dsc::paramStr(p, "ascii", "0") == "1");
                       });
} // namespace
