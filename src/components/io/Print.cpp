// Print — 仿真监控元件实现
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Print.h"
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

// 字符模式：串口字节流要的是原始字符，不能走上面那种十六进制格式。
// 遇换行立刻刷出，否则重定向到文件时整段输出要等缓冲满才可见
extern "C" void dcs_print_char(unsigned char c) {
    std::fputc((int) c, stdout);
    if (c == '\n')
        std::fflush(stdout);
}

Print::Print(const std::string &name, int bit_width, bool ascii) :
    SequentialComponent(name, "print"), _bit_width(bit_width), _ascii(ascii) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("ascii", ascii ? "1" : "0");
    addInput("in", bit_width);
    addInput("clk", 1);
    // 门控输入：只在该信号为高的拍交付。不接时按恒使能处理，
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

    // 字符模式下 valid 往往只高一拍，用上升沿触发保证一个字节只打一次；
    // 十六进制模式沿用原来的"每个 clk 上升沿打一次"，避免改变既有行为
    std::string emit = _ascii
                           ? std::format("    if (_en_rising) dcs_print_char((unsigned char) (_val & 0xFF));")
                           : std::format("    if (_rising && _en) dcs_print(\"{}\", (const uint8_t*)&_val, {});", name(), bytes);

    return std::format(R"(extern void dcs_print(const char*, const uint8_t*, int);
extern void dcs_print_char(unsigned char);
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

std::unique_ptr<Component> Print::clone(const std::string &n) const {
    return std::make_unique<Print>(n, _bit_width, _ascii);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"print", {{"bit_width", "8"}, {"ascii", "0", {"0", "1"}}}, {"in", "clk", "en"}, {}, dsc::ComponentCategory::Misc, "整型打印，clk上升沿打印当前值（en 为高才打印，ascii=1 时按字符输出）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Print>(n, dsc::paramInt(p, "bit_width", 8), dsc::paramStr(p, "ascii", "0") == "1");
                       });
} // namespace
