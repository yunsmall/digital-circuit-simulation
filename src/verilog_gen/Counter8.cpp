// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#include "dcs/components/verilog/Counter8.h"

#include "Vcounter8.h"
#include "dcs/ComponentFactory.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <verilated.h>

namespace dsc {

struct Counter8::State {
    VerilatedContext ctx;
    std::unique_ptr<Vcounter8> model;
};

// __en 是按位给出驱动使能的，而 dcs 的三态是整根线驱动；只要有一位使能就整根线驱动。
// 全 0 / 全 1 是常见情形，逐位混合的驱动 dcs 表达不了
static bool dcsAnyBits(const void *p, std::size_t n) {
    auto *b = static_cast<const unsigned char *>(p);
    for (std::size_t i = 0; i < n; i++)
        if (b[i])
            return true;
    return false;
}

// 端口在 Verilator 模型里是引用成员（VL_IN8 展开成 "CData &a"），
// 没法用对象内偏移访问，只能每拍走一次这个回调搬引脚。
// 每个端口在缓冲区里占固定 16 字节（dcs 线网槽宽度），窄端口只用到前几字节。
// pseudo 标记哪些输入接在"每 tick 一个沿"的伪时钟线上：这些线电平恒 0，
// 必须在这里合成时钟跳变，否则模型永远看不到沿
static void dcsEvalCounter8(void *p, const unsigned char *in, unsigned char *out,
                          const unsigned char *pseudo) {
    auto *m = static_cast<Vcounter8 *>(p);
    std::memcpy(&m->clk, in + 0, 1);
    std::memcpy(&m->rst, in + 16, 1);
    std::memcpy(&m->en, in + 32, 1);
    if (!(pseudo[0] != 0)) {
        m->eval();
    } else {
        // 多个伪时钟在同一拍里一起翻转：先全部置非活跃电平，eval 让模型记录
        // "沿前"状态；再全部置活跃电平，eval 产生沿
        if (pseudo[0]) m->clk = 0;
        m->eval();
        if (pseudo[0]) m->clk = 1;
        m->eval();
    }
    std::memcpy(out + 0, &m->count, 1);
}

Counter8::Counter8(const std::string &name) :
    Component(name, "gen_counter8"), _state(std::make_unique<State>()) {
    _state->model = std::make_unique<Vcounter8>(&_state->ctx, "counter8");
    addInput("clk", 1, PinEdge::Rising);
    addInput("rst", 1);
    addInput("en", 1);
    addOutput("count", 8, false, true);
}

Counter8::~Counter8() = default;

std::vector<Component::JitSymbol> Counter8::extraJitSymbols() const {
    return {{"dcs_eval_gen_counter8", reinterpret_cast<void *>(&dcsEvalCounter8)}};
}

std::string Counter8::genStateDecl() const {
    // 伪时钟掩码在生成 C 代码时才知道（接线是 compile 前定的）：哪些输入接在
    // 伪时钟线上，回调就要为它们合成沿。数组按实例生成，回调是类型级共享的
    std::string mask;
    for (int i = 0; i < (int) inputs().size(); i++) {
        const Net *n = inputs()[i]->net();
        mask += std::format("{}{}", i ? ", " : "", (n && n->pseudoEdge() != PinEdge::None) ? 1 : 0);
    }
    std::string c = std::format(
            "static void *_vmd_{0} = (void *)0x{1:X}ULL;\n",
            _id, (unsigned long long) (std::uintptr_t) _state->model.get());
    c += std::format("static const unsigned char _vpm_{0}[{1}] = {{{2}}};\n",
                     _id, inputs().empty() ? 1 : (int) inputs().size(),
                     mask.empty() ? "0" : mask);
    c += std::format(
            "extern void dcs_eval_gen_counter8(void *, const unsigned char *, unsigned char *, "
            "const unsigned char *);\n");
    return c;
}

std::string Counter8::genFuncDef_seq() const {
    std::string c;
    c += std::format("static void {}() {{\n", funcName_seq());
    c += std::format("    static unsigned char _vio_in_{0}[48];\n", _id);
    c += std::format("    static unsigned char _vio_out_{0}[16];\n", _id);
    c += std::format("    dcs_memcpy(_vio_in_{0} + 0, _w[{1}], 1);\n", _id, inputs()[0]->netId());
    c += std::format("    dcs_memcpy(_vio_in_{0} + 16, _w[{1}], 1);\n", _id, inputs()[1]->netId());
    c += std::format("    dcs_memcpy(_vio_in_{0} + 32, _w[{1}], 1);\n", _id, inputs()[2]->netId());
    c += std::format("    dcs_eval_gen_counter8(_vmd_{0}, _vio_in_{0}, _vio_out_{0}, _vpm_{0});\n", _id);
    c += "    " + genOutputWrite(0, std::format("_vio_out_{}[0]", _id), 8) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Counter8::clone(const std::string &new_name) const {
    return std::make_unique<Counter8>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"gen_counter8", {}, {"clk", "rst", "en"}, {"count"}, dsc::ComponentCategory::Misc,
     "由 Verilog 模块 counter8 自动生成的元件"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::Counter8>(n);
    });
} // namespace
