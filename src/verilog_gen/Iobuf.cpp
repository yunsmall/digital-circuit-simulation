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

// 端口在 Verilator 模型里是引用成员（VL_IN8 展开成 "CData &a"），
// 没法用对象内偏移访问，只能每拍走一次这个回调搬引脚。
// 每个端口在缓冲区里占固定 16 字节（dcs 线网槽宽度），窄端口只用到前几字节
static void dcsEvalIobuf(void *p, const unsigned char *in, unsigned char *out) {
    auto *m = static_cast<Viobuf *>(p);
    std::memcpy(&m->dout, in + 0, 1);
    std::memcpy(&m->oe, in + 16, 1);
    m->eval();
    std::memcpy(out + 0, &m->dout, 1);
    out[16] = (unsigned char) m->oe;
}

Iobuf::Iobuf(const std::string &name) :
    Component(name, "gen_iobuf"), _state(std::make_unique<State>()) {
    _state->model = std::make_unique<Viobuf>(&_state->ctx, "iobuf");
    addInput("dout", 8);
    addInput("oe", 1);
    addOutput("pad", 8, true);
}

Iobuf::~Iobuf() = default;

std::vector<Component::JitSymbol> Iobuf::extraJitSymbols() const {
    return {{"dcs_eval_gen_iobuf", reinterpret_cast<void *>(&dcsEvalIobuf)}};
}

std::string Iobuf::genStateDecl() const {
    return std::format(
            "static void *_vmd_{0} = (void *)0x{1:X}ULL;\n"
            "extern void dcs_eval_gen_iobuf(void *, const unsigned char *, unsigned char *);\n",
            _id, (unsigned long long) (std::uintptr_t) _state->model.get());
}

std::string Iobuf::genFuncDef_comb() const {
    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_comb());
    c += std::format("    static unsigned char _vio_in_{0}[32];\n", _id);
    c += std::format("    static unsigned char _vio_out_{0}[32];\n", _id);
    c += std::format("    dcs_memcpy(_vio_in_{0} + 0, _w[{1}], 1);\n", _id, inputs()[0]->netId());
    c += std::format("    dcs_memcpy(_vio_in_{0} + 16, _w[{1}], 1);\n", _id, inputs()[1]->netId());
    c += std::format("    dcs_eval_gen_iobuf(_vmd_{0}, _vio_in_{0}, _vio_out_{0});\n", _id);
    c += std::format("    const uint8_t _oe0 = _vio_out_{0}[16];\n", _id);
    c += "    " + genOutputWrite(0, std::format("_vio_out_{}[0]", _id), 8, "_oe0") + "\n";
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
    {"gen_iobuf", {}, {"dout", "oe"}, {"pad"}, dsc::ComponentCategory::Misc,
     "由 Verilog 模块 iobuf 自动生成的元件"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::Iobuf>(n);
    });
} // namespace
