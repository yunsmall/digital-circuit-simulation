#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Display.h"

#include <format>
#include <stdexcept>

namespace dsc {

struct Display::State {
    uint64_t value = 0;
};

namespace {

const char *kindKey(Display::Kind k) {
    switch (k) {
        case Display::Kind::Led:
            return "led";
        case Display::Kind::SevenSeg:
            return "seg";
        case Display::Kind::Text:
            return "text";
    }
    return "led";
}

} // namespace

// JIT 在值变化时才跨到这里；同一个函数被所有 Display 实例共用，靠 owner 区分
extern "C" void dcs_display_update(void *owner, unsigned long long v) {
    static_cast<Display *>(owner)->notifyFromJit((uint64_t) v);
}

extern "C" void dcs_display_reset(void *owner) {
    static_cast<Display *>(owner)->resetHostValue();
}

void Display::setSink(const Sink &sink, void *ctx) {
    _sink = sink;
    _sink_ctx = ctx;
}

Display::Display(const std::string &name, int bit_width, Kind kind, int digits) :
    SequentialComponent(name, "display"),
    _state(std::make_unique<State>()),
    _bit_width(bit_width),
    _kind(kind),
    _digits(digits) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    if (digits < 1 || digits > 8)
        throw std::invalid_argument("数码管位数必须在1-8之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("kind", kindKey(kind));
    setParam("digits", std::to_string(digits));
    addInput("in", bit_width);
}

Display::~Display() = default;

uint64_t Display::value() const {
    return _state->value;
}

void Display::notifyFromJit(uint64_t v) {
    _state->value = v;
    if (_sink.on_change)
        _sink.on_change(_sink_ctx, this, v);
}

void Display::resetHostValue() {
    _state->value = 0;
}

std::string Display::genStructDef() const {
    return std::format("typedef struct {{\n    uint64_t last;\n    uint8_t valid;\n}} {};", stateTypeName());
}

std::string Display::genStateDecl() const {
    return std::format("static {} {};\nstatic void *_disp_owner_{};", stateTypeName(), stateVarName(), _id);
}

std::string Display::genInitCode() const {
    return std::format("    {}.last = 0; {}.valid = 0; _disp_owner_{} = (void *)0x{:X}ULL;", stateVarName(), stateVarName(),
                       _id, (unsigned long long) reinterpret_cast<uintptr_t>(this));
}

std::string Display::genResetCode() const {
    // circuit_reset 已经把线网清零；这里把宿主侧 value() 缓存也归零，
    // 否则 reset 后到下一次 tick 之间调用 value() 还能读到复位前的旧值
    return genInitCode() +
           std::format("    extern void dcs_display_reset(void *);\n"
                       "    dcs_display_reset((void *)0x{:X}ULL);\n",
                       (unsigned long long) reinterpret_cast<uintptr_t>(this));
}

std::string Display::genFuncDef_seq() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    auto st = stateVarName();

    std::string c;
    c += "extern void dcs_display_update(void *, unsigned long long);\n";
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    " + gen_read_wire(in_nid, _bit_width, in_nw, "_raw") + "\n";
    c += "    const uint64_t _val = (uint64_t) _raw;\n";
    // 只有真的变了才进宿主，静止画面的显示件每拍都零开销
    c += std::format("    if (!{0}.valid || {0}.last != _val) {{\n", st);
    c += std::format("        {0}.valid = 1; {0}.last = _val;\n", st);
    c += std::format("        dcs_display_update(_disp_owner_{}, _val);\n", _id);
    c += "    }\n";
    c += "}\n";
    return c;
}

std::vector<Component::JitSymbol> Display::extraJitSymbols() const {
    return {{"dcs_display_update", reinterpret_cast<void *>(&dcs_display_update)},
            {"dcs_display_reset", reinterpret_cast<void *>(&dcs_display_reset)}};
}

std::unique_ptr<Component> Display::clone(const std::string &n) const {
    return std::make_unique<Display>(n, _bit_width, _kind, _digits);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"display", {{"bit_width", "8"}, {"kind", "led", {"led", "seg", "text"}}, {"digits", "1"}}, {"in"}, {},
     dsc::ComponentCategory::Misc,
     "显示件：值变化时把新值交给宿主（每个实例单独 setSink 绑定）；kind=led/seg/text"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           const std::string k = dsc::paramStr(p, "kind", "led");
                           const auto kind = k == "seg"    ? dsc::Display::Kind::SevenSeg
                                             : k == "text" ? dsc::Display::Kind::Text
                                                           : dsc::Display::Kind::Led;
                           return std::make_unique<dsc::Display>(n, dsc::paramInt(p, "bit_width", 8), kind,
                                                                 dsc::paramInt(p, "digits", 1));
                       });
} // namespace
