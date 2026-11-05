#include "dcs/ComponentFactory.h"
#include "dcs/components/sequential/RingCounter.h"

#include <format>
#include <stdexcept>

namespace dsc {

RingCounter::RingCounter(const std::string &name, int length, bool johnson) :
    SequentialComponent(name, "ringcounter"), _length(length), _johnson(johnson) {
    if (length < 2 || length > 64)
        throw std::invalid_argument("位数必须在2-64之间");
    setParam("length", std::to_string(length));
    setParam("johnson", johnson ? "1" : "0");
    addInput("clk", 1);
    addInput("en", 1);
    addInput("rst", 1);
    addOutput("q", length, false, true);
}

std::string RingCounter::genStructDef() const {
    return std::format("typedef struct {{\n    uint64_t q;\n    uint8_t prev_clk;\n}} {};", stateTypeName());
}

std::string RingCounter::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string RingCounter::genInitCode() const {
    auto st = stateVarName();
    // 环形从最低位 one-hot 起步；约翰逊从全 0 起步，否则跑不出完整 2N 状态
    const uint64_t init = _johnson ? 0 : 1;
    return std::format("    {}.q = {}ULL; {}.prev_clk = 0;", st, init, st);
}

std::string RingCounter::genFuncDef_seq() const {
    int clk_nid = inputs()[0]->netId();
    int en_nid = inputs()[1]->netId();
    int rst_nid = inputs()[2]->netId();
    int en_nw = en_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int rst_nw = rst_nid >= 0 ? inputs()[2]->net()->bit_width() : 0;
    auto st = stateVarName();
    const int msb = _length - 1;

    std::string c;
    c += std::format("static void {}() {{\n", funcName_seq());
    c += clk_nid >= 0 ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);\n", clk_nid)
                      : std::string("    bool _clk = false;\n");
    c += "    " + gen_read_wire(en_nid, 1, en_nw, "_en") + "\n";
    c += "    " + gen_read_wire(rst_nid, 1, rst_nw, "_rst") + "\n";
    c += std::format("    if (_rst & 1) {{\n", st);
    c += std::format("        {}.q = {}ULL;\n", st, _johnson ? 0 : 1);
    // 复位是异步的，和 DFF/Register/ShiftRegister 一致：置 1 当拍就回到初值，
    // 不等时钟沿。放在沿内会让短于一个周期的复位脉冲丢失
    c += std::format("    }} else if (_clk && !{}.prev_clk) {{\n", st);
    c += std::format("        if (_en & 1) {{\n");
    // 反馈位就是最高位；约翰逊取反，状态数翻倍
    c += std::format("            const uint64_t _fb = (({}.q >> {}) & 1ULL){};\n", st, msb, _johnson ? "^ 1ULL" : "");
    c += std::format("            {}.q = (({}.q << 1) | _fb) & {};\n", st, st, gen_mask(_length));
    c += "        }\n";
    c += "    }\n";
    c += std::format("    {}.prev_clk = _clk;\n", st);
    c += std::format("    uint64_t _out = {}.q;\n", st);
    c += "    " + genOutputWrite(0, "_out", _length) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> RingCounter::clone(const std::string &n) const {
    return std::make_unique<RingCounter>(n, _length, _johnson);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"ringcounter", {{"length", "8"}, {"johnson", "0", {"0", "1"}}}, {"clk", "en", "rst"}, {"q"},
     dsc::ComponentCategory::Sequential, "环形/约翰逊计数器：复位后环形为最低位 one-hot，约翰逊为全 0"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::RingCounter>(n, dsc::paramInt(p, "length", 8),
                                                  dsc::paramInt(p, "johnson", 0) != 0);
    });
} // namespace
