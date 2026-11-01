#include "dcs/ComponentFactory.h"
#include "dcs/components/memory/RegFile.h"

#include <format>
#include <stdexcept>

namespace dsc {

RegFile::RegFile(const std::string &name, int bit_width, int depth, int num_read_ports, int num_write_ports) :
    Component(name, "regfile"),
    _bit_width(bit_width),
    _depth(depth),
    _read_ports(num_read_ports),
    _write_ports(num_write_ports) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    if (depth < 2 || depth > 256)
        throw std::invalid_argument("寄存器数量必须在2-256之间");
    if (num_read_ports < 1 || num_read_ports > 4)
        throw std::invalid_argument("读口数量必须在1-4之间");
    if (num_write_ports < 1 || num_write_ports > 2)
        throw std::invalid_argument("写口数量必须在1-2之间");

    setParam("bit_width", std::to_string(bit_width));
    setParam("depth", std::to_string(depth));
    setParam("num_read_ports", std::to_string(num_read_ports));
    setParam("num_write_ports", std::to_string(num_write_ports));

    const int ab = addrBits();
    addInput("clk", 1);
    for (int i = 0; i < _write_ports; i++) {
        addInput(std::format("we{}", i), 1);
        addInput(std::format("waddr{}", i), ab);
        addInput(std::format("wdata{}", i), bit_width);
    }
    for (int i = 0; i < _read_ports; i++)
        addInput(std::format("raddr{}", i), ab);
    for (int i = 0; i < _read_ports; i++)
        addOutput(std::format("rdata{}", i), bit_width);
}

int RegFile::addrBits() const {
    int bits = 1;
    while ((1 << bits) < _depth)
        bits++;
    return bits;
}

std::string RegFile::genStructDef() const {
    return std::format("typedef struct {{\n    uint64_t regs[{}];\n    uint8_t prev_clk;\n}} {};", _depth,
                       stateTypeName());
}

std::string RegFile::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string RegFile::genInitCode() const {
    auto st = stateVarName();
    return std::format("    dcs_memset({}.regs, 0, sizeof({}.regs)); {}.prev_clk = 0;", st, st, st);
}

std::string RegFile::genFuncDef_comb() const {
    const int ab = addrBits();
    const int base = 1 + _write_ports * 3;
    auto st = stateVarName();
    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_comb());
    for (int i = 0; i < _read_ports; i++) {
        const int idx = base + i;
        int nid = inputs()[idx]->netId();
        int nw = nid >= 0 ? inputs()[idx]->net()->bit_width() : 0;
        c += "    " + gen_read_wire(nid, ab, nw, std::format("_ra{}", i)) + "\n";
        c += std::format("    uint64_t _r{0} = ((uint64_t) _ra{0} < {1}u) ? {2}.regs[_ra{0}] : 0ULL;\n", i, _depth, st);
        c += "    " + genOutputWrite(i, std::format("_r{}", i), _bit_width) + "\n";
    }
    c += "}\n";
    return c;
}

std::string RegFile::genFuncDef_seq() const {
    const int ab = addrBits();
    auto st = stateVarName();
    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    int clk_nid = inputs()[0]->netId();
    c += clk_nid >= 0 ? std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);\n", clk_nid)
                      : std::string("    bool _clk = false;\n");
    c += std::format("    if (_clk && !{}.prev_clk) {{\n", st);
    for (int i = 0; i < _write_ports; i++) {
        const int we_i = 1 + i * 3;
        const int wa_i = we_i + 1;
        const int wd_i = we_i + 2;
        int we_nid = inputs()[we_i]->netId();
        int wa_nw = inputs()[wa_i]->netId() >= 0 ? inputs()[wa_i]->net()->bit_width() : 0;
        int wd_nw = inputs()[wd_i]->netId() >= 0 ? inputs()[wd_i]->net()->bit_width() : 0;
        c += "        " + gen_read_wire(inputs()[we_i]->netId(), 1, we_nid >= 0 ? inputs()[we_i]->net()->bit_width() : 0,
                                        std::format("_we{}", i)) + "\n";
        c += "        " + gen_read_wire(inputs()[wa_i]->netId(), ab, wa_nw, std::format("_wa{}", i)) + "\n";
        c += "        " + gen_read_wire(inputs()[wd_i]->netId(), _bit_width, wd_nw, std::format("_wd{}", i)) + "\n";
        c += std::format("        if ((_we{0} & 1) && (uint64_t) _wa{0} < {1}u)\n", i, _depth);
        c += std::format("            {0}.regs[_wa{1}] = (uint64_t) _wd{1} & {2};\n", st, i, gen_mask(_bit_width));
    }
    c += "    }\n";
    c += std::format("    {}.prev_clk = _clk;\n", st);
    c += "}\n";
    return c;
}

std::unique_ptr<Component> RegFile::clone(const std::string &n) const {
    return std::make_unique<RegFile>(n, _bit_width, _depth, _read_ports, _write_ports);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"regfile",
     {{"bit_width", "32"}, {"depth", "32"}, {"num_read_ports", "2"}, {"num_write_ports", "1"}},
     {"clk"}, {"rdata0"}, dsc::ComponentCategory::Memory, "多读口寄存器堆：组合读、时钟沿写，越界地址读出 0"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::RegFile>(n, dsc::paramInt(p, "bit_width", 32), dsc::paramInt(p, "depth", 32),
                                              dsc::paramInt(p, "num_read_ports", 2),
                                              dsc::paramInt(p, "num_write_ports", 1));
    });
} // namespace
