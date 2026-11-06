// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#include "dcs/components/verilog/Iobuf.h"

#include "Viobuf.h"
#include "dcs/ComponentFactory.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <verilated.h>

namespace dsc {

struct Iobuf::State {
    VerilatedContext ctx;
    std::unique_ptr<Viobuf> model;
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
static void dcsEvalIobuf(void *p, const unsigned char *in, unsigned char *out,
                          const unsigned char *pseudo) {
    auto *m = static_cast<Viobuf *>(p);
    std::memcpy(&m->dout, in + 0, 1);
    std::memcpy(&m->oe, in + 16, 1);
    std::memcpy(&m->pad, in + 32, 1);
    if (!(false)) {
        m->eval();
    } else {
        // 多个伪时钟在同一拍里一起翻转：先全部置非活跃电平，eval 让模型记录
        // "沿前"状态；再全部置活跃电平，eval 产生沿
        // 没有时钟输入
        m->eval();
        // 没有时钟输入
        m->eval();
    }
    std::memcpy(out + 0, &m->pad__out, 1);
    out[16] = dcsAnyBits(&m->pad__en, 1);
    std::memcpy(out + 32, &m->din, 1);
}

Iobuf::Iobuf(const std::string &name) :
    Component(name, "gen_iobuf"), _state(std::make_unique<State>()) {
    _state->model = std::make_unique<Viobuf>(&_state->ctx, "iobuf");
    addInput("dout", 8);
    addInput("oe", 1);
    addInput("pad_i", 8);
    addOutput("pad", 8, true);
    addOutput("din", 8);
}

Iobuf::~Iobuf() = default;

std::vector<Component::JitSymbol> Iobuf::extraJitSymbols() const {
    return {{"dcs_eval_gen_iobuf", reinterpret_cast<void *>(&dcsEvalIobuf)}};
}

std::string Iobuf::genStateDecl() const {
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
            "extern void dcs_eval_gen_iobuf(void *, const unsigned char *, unsigned char *, "
            "const unsigned char *);\n");
    return c;
}

std::string Iobuf::genFuncDef_comb() const {
    std::string c;
    c += std::format("static void {}() {{\n", funcName_comb());
    c += std::format("    static unsigned char _vio_in_{0}[48];\n", _id);
    c += std::format("    static unsigned char _vio_out_{0}[48];\n", _id);
    c += std::format("    dcs_memcpy(_vio_in_{0} + 0, _w[{1}], 1);\n", _id, inputs()[0]->netId());
    c += std::format("    dcs_memcpy(_vio_in_{0} + 16, _w[{1}], 1);\n", _id, inputs()[1]->netId());
    c += std::format("    dcs_memcpy(_vio_in_{0} + 32, _w[{1}], 1);\n", _id, inputs()[2]->netId());
    c += std::format("    dcs_eval_gen_iobuf(_vmd_{0}, _vio_in_{0}, _vio_out_{0}, _vpm_{0});\n", _id);
    c += std::format("    const uint8_t _oe0 = _vio_out_{0}[16];\n", _id);
    c += "    " + genOutputWrite(0, std::format("_vio_out_{}[0]", _id), 8, "_oe0") + "\n";
    c += "    " + genOutputWrite(1, std::format("_vio_out_{}[32]", _id), 8) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Iobuf::clone(const std::string &new_name) const {
    return std::make_unique<Iobuf>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"gen_iobuf", {}, {"dout", "oe", "pad_i"}, {"pad", "din"}, dsc::ComponentCategory::Misc,
     "由 Verilog 模块 iobuf 自动生成的元件"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::Iobuf>(n);
    });
} // namespace
