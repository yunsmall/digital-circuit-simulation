#include "dcs/ComponentFactory.h"
#include "dcs/components/io/Display.h"

#include <format>
#include <stdexcept>

namespace dsc {

struct Display::State {
    uint64_t value = 0;
};

namespace {

Display::Sink &sinkSlot() {
    static Display::Sink s{};
    return s;
}

void *&sinkCtxSlot() {
    static void *ctx = nullptr;
    return ctx;
}

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

void Display::setSink(const Sink &sink, void *ctx) {
    sinkSlot() = sink;
    sinkCtxSlot() = ctx;
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
    auto &s = sinkSlot();
    if (s.on_change)
        s.on_change(sinkCtxSlot(), this, v);
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

std::string Display::genFuncDef_seq() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    auto st = stateVarName();

    std::string c;
    c += "extern void dcs_display_update(void *, unsigned long long);\n";
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
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
    return {{"dcs_display_update", reinterpret_cast<void *>(&dcs_display_update)}};
}

std::unique_ptr<Component> Display::clone(const std::string &n) const {
    return std::make_unique<Display>(n, _bit_width, _kind, _digits);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"display", {{"bit_width", "8"}, {"kind", "led", {"led", "seg", "text"}}, {"digits", "1"}}, {"in"}, {},
     dsc::ComponentCategory::Misc,
     "显示件：值变化时把新值交给宿主（GUI 用 Display::setSink 接入）；kind=led/seg/text"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           const std::string k = dsc::paramStr(p, "kind", "led");
                           const auto kind = k == "seg"    ? dsc::Display::Kind::SevenSeg
                                             : k == "text" ? dsc::Display::Kind::Text
                                                           : dsc::Display::Kind::Led;
                           return std::make_unique<dsc::Display>(n, dsc::paramInt(p, "bit_width", 8), kind,
                                                                 dsc::paramInt(p, "digits", 1));
                       });
} // namespace
