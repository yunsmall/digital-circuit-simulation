// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#include "dcs/components/verilog/Counter8.h"

#include "Vcounter8.h"
#include "dcs/ComponentFactory.h"

#include <cstdint>
#include <format>
#include <memory>
#include <verilated.h>

namespace dsc {

struct Counter8::State {
    VerilatedContext ctx;
    std::unique_ptr<Vcounter8> model;
};

// 端口在 Verilator 模型里是引用成员（VL_IN8 展开成 "CData &a"），
// 没法用对象内偏移访问，只能每拍走一次这个回调搬引脚
static void dcsEvalCounter8(void *p, const uint64_t *in, uint64_t *out) {
    auto *m = static_cast<Vcounter8 *>(p);
    m->clk = (uint8_t) in[0];
    m->rst = (uint8_t) in[1];
    m->en = (uint8_t) in[2];
    m->eval();
    out[0] = (uint8_t) m->count;
}

Counter8::Counter8(const std::string &name) :
    Component(name, "gen_counter8"), _state(std::make_unique<State>()) {
    _state->model = std::make_unique<Vcounter8>(&_state->ctx, "counter8");
    addInput("clk", 1);
    addInput("rst", 1);
    addInput("en", 1);
    addOutput("count", 8, false, true);
}

Counter8::~Counter8() = default;

std::vector<Component::JitSymbol> Counter8::extraJitSymbols() const {
    return {{"dcs_eval_gen_counter8", reinterpret_cast<void *>(&dcsEvalCounter8)}};
}

std::string Counter8::genStateDecl() const {
    return std::format(
            "static void *_vmd_{0} = (void *)0x{1:X}ULL;\n"
            "extern void dcs_eval_gen_counter8(void *, const uint64_t *, uint64_t *);\n",
            _id, (unsigned long long) (std::uintptr_t) _state->model.get());
}

std::string Counter8::genFuncDef_seq() const {
    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    c += std::format("    static uint64_t _vio_in_{0}[3];\n", _id);
    c += std::format("    static uint64_t _vio_out_{0}[1];\n", _id);
    c += std::format("    uint64_t _i0 = 0; dcs_memcpy(&_i0, _w[{1}], 1); _vio_in_{0}[0] = _i0;\n", _id, inputs()[0]->netId());
    c += std::format("    uint64_t _i1 = 0; dcs_memcpy(&_i1, _w[{1}], 1); _vio_in_{0}[1] = _i1;\n", _id, inputs()[1]->netId());
    c += std::format("    uint64_t _i2 = 0; dcs_memcpy(&_i2, _w[{1}], 1); _vio_in_{0}[2] = _i2;\n", _id, inputs()[2]->netId());
    c += std::format("    dcs_eval_gen_counter8(_vmd_{0}, _vio_in_{0}, _vio_out_{0});\n", _id);
    c += std::format("    const uint8_t _o0 = (uint8_t) _vio_out_{0}[0];\n", _id);
    c += "    " + genOutputWrite(0, "_o0", 8) + "\n";
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
