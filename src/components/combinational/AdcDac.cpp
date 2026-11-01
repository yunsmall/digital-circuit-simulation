#include "dcs/ComponentFactory.h"
#include "dcs/components/combinational/AdcDac.h"

#include <format>
#include <stdexcept>

namespace dsc {

namespace {
// 满量程数字值 2^n - 1，用 double 表示避免 32 位时的精度问题
std::string full_scale(int bit_width) {
    return std::format("{}.0", (bit_width >= 32) ? 4294967295.0 : (double) ((1ull << bit_width) - 1));
}
} // namespace

Dac::Dac(const std::string &name, int bit_width, double vref) :
    CombinationalComponent(name, "dac"), _bit_width(bit_width), _vref(vref) {
    if (bit_width < 1 || bit_width > 32)
        throw std::invalid_argument("位宽必须在1-32之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("vref", std::format("{}", vref));
    addInput("value", bit_width);
    addOutput("analog", 32); // float 的位模式
}

std::string Dac::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    std::string c;
    c += std::format("static void {}() {{\n", funcName_comb());
    c += "    " + gen_read_wire(in_nid, _bit_width, in_nw, "_v") + "\n";
    c += std::format("    const double _full = {};\n", full_scale(_bit_width));
    c += std::format("    const double _vref = {};\n", _vref);
    // 先按 double 算再落到 float，避免整数除法或中间精度不够
    c += "    const float _analog = (float) (((double) _v) / _full * _vref);\n";
    c += "    uint32_t _bits = 0; dcs_memcpy(&_bits, &_analog, 4);\n";
    c += "    " + genOutputWrite(0, "_bits", 32) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Dac::clone(const std::string &n) const {
    return std::make_unique<Dac>(n, _bit_width, _vref);
}

Adc::Adc(const std::string &name, int bit_width, double vref) :
    CombinationalComponent(name, "adc"), _bit_width(bit_width), _vref(vref) {
    if (bit_width < 1 || bit_width > 32)
        throw std::invalid_argument("位宽必须在1-32之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("vref", std::format("{}", vref));
    addInput("analog", 32);
    addOutput("value", bit_width);
}

std::string Adc::genFuncDef_comb() const {
    int in_nid = inputs()[0]->netId();
    int in_nw = in_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    std::string c;
    c += std::format("static void {}() {{\n", funcName_comb());
    c += "    " + gen_read_wire(in_nid, 32, in_nw, "_inbits") + "\n";
    c += std::format("    const double _full = {};\n", full_scale(_bit_width));
    c += std::format("    const double _vref = {};\n", _vref);
    c += std::format("    const uint64_t _max = {}ULL;\n", (1ull << _bit_width) - 1);
    c += "    float _analog = 0; dcs_memcpy(&_analog, &_inbits, 4);\n";
    c += "    uint64_t _out = 0;\n";
    // vref 为 0 时没有可定义的量化关系，直接给 0，避免除零产生 inf 再转整数是 UB
    c += "    if (_vref > 0.0) {\n";
    c += "        double _q = (double) _analog / _vref * _full;\n";
    c += "        if (_q < 0.0) _q = 0.0; // 负电压和超量程都饱和，不让整数转换越界\n";
    c += "        else if (_q > _full) _q = _full;\n";
    // 先转有符号再转无符号：值域上界是 2^32-1，远在 int64 之内。TCC 的内存编译
    // 没有链接 libtcc1.a，double→uint64 会留下 __fixunsdfdi 未解析；有符号转换
    // 是 x86 的原生指令，不产生库调用
    c += "        _out = (uint64_t) (int64_t) (_q + 0.5);\n";
    c += "        if (_out > _max) _out = _max;\n";
    c += "    }\n";
    c += "    " + genOutputWrite(0, "_out", _bit_width) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Adc::clone(const std::string &n) const {
    return std::make_unique<Adc>(n, _bit_width, _vref);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"dac", {{"bit_width", "8"}, {"vref", "5.0"}}, {"value"}, {"analog"}, dsc::ComponentCategory::Float,
     "数模转换：数字量线性映射到 [0, vref]，analog 是 float 位模式"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::Dac>(n, dsc::paramInt(p, "bit_width", 8), std::stod(dsc::paramStr(p, "vref", "5.0")));
    });

DCS_REGISTER_COMPONENT(
    {"adc", {{"bit_width", "8"}, {"vref", "5.0"}}, {"analog"}, {"value"}, dsc::ComponentCategory::Float,
     "模数转换：float 电压量化到数字量，越界饱和到 0 / 满量程"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::Adc>(n, dsc::paramInt(p, "bit_width", 8), std::stod(dsc::paramStr(p, "vref", "5.0")));
    });
} // namespace
