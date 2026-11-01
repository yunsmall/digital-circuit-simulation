// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#include "dcs/components/verilog/Wide128.h"

#include "Vwide128.h"
#include "dcs/ComponentFactory.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <verilated.h>

namespace dsc {

struct Wide128::State {
    VerilatedContext ctx;
    std::unique_ptr<Vwide128> model;
};

// 端口在 Verilator 模型里是引用成员（VL_IN8 展开成 "CData &a"），
// 没法用对象内偏移访问，只能每拍走一次这个回调搬引脚。
// 每个端口在缓冲区里占固定 16 字节（dcs 线网槽宽度），窄端口只用到前几字节
static void dcsEvalWide128(void *p, const unsigned char *in, unsigned char *out) {
    auto *m = static_cast<Vwide128 *>(p);
    std::memcpy(&m->a, in + 0, 16);
    m->eval();
    std::memcpy(out + 0, &m->y, 16);
}

Wide128::Wide128(const std::string &name) :
    Component(name, "gen_wide128"), _state(std::make_unique<State>()) {
    _state->model = std::make_unique<Vwide128>(&_state->ctx, "wide128");
    addInput("a", 128);
    addOutput("y", 128);
}

Wide128::~Wide128() = default;

std::vector<Component::JitSymbol> Wide128::extraJitSymbols() const {
    return {{"dcs_eval_gen_wide128", reinterpret_cast<void *>(&dcsEvalWide128)}};
}

std::string Wide128::genStateDecl() const {
    return std::format(
            "static void *_vmd_{0} = (void *)0x{1:X}ULL;\n"
            "extern void dcs_eval_gen_wide128(void *, const unsigned char *, unsigned char *);\n",
            _id, (unsigned long long) (std::uintptr_t) _state->model.get());
}

std::string Wide128::genFuncDef_comb() const {
    std::string c;
    c += std::format("static void {}() {{\n", funcName_comb());
    c += std::format("    static unsigned char _vio_in_{0}[16];\n", _id);
    c += std::format("    static unsigned char _vio_out_{0}[16];\n", _id);
    c += std::format("    dcs_memcpy(_vio_in_{0} + 0, _w[{1}], 16);\n", _id, inputs()[0]->netId());
    c += std::format("    dcs_eval_gen_wide128(_vmd_{0}, _vio_in_{0}, _vio_out_{0});\n", _id);
    c += "    " + genOutputWrite(0, std::format("_vio_out_{}[0]", _id), 128) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Wide128::clone(const std::string &new_name) const {
    return std::make_unique<Wide128>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"gen_wide128", {}, {"a"}, {"y"}, dsc::ComponentCategory::Misc,
     "由 Verilog 模块 wide128 自动生成的元件"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::Wide128>(n);
    });
} // namespace
