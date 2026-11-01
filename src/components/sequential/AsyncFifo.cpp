#include "dcs/ComponentFactory.h"
#include "dcs/components/sequential/AsyncFifo.h"

#include <format>
#include <stdexcept>

namespace dsc {

AsyncFifo::AsyncFifo(const std::string &name, int bit_width, int depth) :
    SequentialComponent(name, "async_fifo"), _bit_width(bit_width), _depth(depth) {
    if (bit_width < 1 || bit_width > 64)
        throw std::invalid_argument("位宽必须在1-64之间");
    if (depth < 2 || depth > 256 || (depth & (depth - 1)) != 0)
        throw std::invalid_argument("深度必须是 2 的幂且在 2-256 之间");
    setParam("bit_width", std::to_string(bit_width));
    setParam("depth", std::to_string(depth));
    addInput("wr_clk", 1);
    addInput("wr_en", 1);
    addInput("wr_data", bit_width);
    addInput("rd_clk", 1);
    addInput("rd_en", 1);
    addOutput("wr_full", 1, false, true);
    addOutput("rd_empty", 1, false, true);
    addOutput("rd_data", bit_width, false, true);
}

std::string AsyncFifo::genStructDef() const {
    return std::format(
            "typedef struct {{\n"
            "    uint64_t mem[{}];\n"
            "    uint32_t wbin, rbin;          // 二进制指针，位宽 = addr_bits + 1\n"
            "    uint32_t wg_s1, wg_s2;        // 读域同步过来的写指针（格雷码）\n"
            "    uint32_t rg_s1, rg_s2;        // 写域同步过来的读指针（格雷码）\n"
            "    uint64_t rdata;\n"
            "    uint8_t prev_wclk, prev_rclk;\n"
            "}} {};",
            _depth, stateTypeName());
}

std::string AsyncFifo::genStateDecl() const {
    return std::format("static {} {};", stateTypeName(), stateVarName());
}

std::string AsyncFifo::genInitCode() const {
    auto st = stateVarName();
    return std::format(
            "    dcs_memset(&{}, 0, sizeof({}));", st, st);
}

std::string AsyncFifo::genFuncDef_seq() const {
    const int addr_bits = [this] {
        int n = 0;
        for (int d = _depth; d > 1; d >>= 1)
            n++;
        return n;
    }();
    const int pw = addr_bits + 1;            // 格雷码指针位宽
    const uint32_t pmask = (1u << pw) - 1u;  // 指针回绕掩码
    const uint32_t amask = (uint32_t) (_depth - 1);
    const uint32_t flip = (uint32_t) (3u << (pw - 2)); // 判满时反转格雷码最高两位
    auto st = stateVarName();

    const auto net = [this](int i) { return inputs()[i]->netId(); };
    const auto nw = [this](int i) { return inputs()[i]->net() ? inputs()[i]->net()->bit_width() : 0; };

    std::string c;
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    c += std::format("    uint8_t _wclk = _w[{}][0] & 1;\n", net(0));
    c += std::format("    uint8_t _wen = _w[{}][0] & 1;\n", net(1));
    c += std::format("    uint8_t _rclk = _w[{}][0] & 1;\n", net(3));
    c += std::format("    uint8_t _ren = _w[{}][0] & 1;\n", net(4));
    c += "    " + gen_read_wire(net(2), _bit_width, nw(2), "_din") + "\n";

    // 满/空都用当前指针和对方同步过来的指针比：满表示"写指针已经领先整整一个深度"，
    // 再写就会覆盖未读数据
    c += std::format("    const uint32_t _wbin = {}.wbin;\n", st);
    c += std::format("    const uint32_t _wg = _wbin ^ (_wbin >> 1);\n");
    c += std::format("    const uint32_t _rg = {}.rbin ^ ({}.rbin >> 1);\n", st, st);
    c += std::format("    const uint32_t _wn = (_wbin + 1) & 0x{:X}u;\n", pmask);
    c += std::format("    const uint8_t _full = (_wg == ({}.rg_s2 ^ 0x{:X}u));\n", st, flip);
    c += std::format("    const uint8_t _empty = (_rg == {}.wg_s2);\n", st);

    // 先读后写：同一拍两个时钟都上升时，读到的应该是写之前的数据
    c += std::format("    if (_rclk && !{}.prev_rclk) {{\n", st);
    c += "        if (_ren && !_empty) {\n";
    c += std::format("            {}.rdata = {}.mem[{}.rbin & 0x{:X}u];\n", st, st, st, amask);
    c += std::format("            {}.rbin = ({}.rbin + 1) & 0x{:X}u;\n", st, st, pmask);
    c += "        }\n";
    c += std::format("        {}.wg_s2 = {}.wg_s1; {}.wg_s1 = _wg;\n", st, st, st);
    c += "    }\n";

    c += std::format("    if (_wclk && !{}.prev_wclk) {{\n", st);
    c += "        if (_wen && !_full) {\n";
    c += std::format("            {}.mem[_wbin & 0x{:X}u] = (uint64_t) _din & {};\n", st, amask, gen_mask(_bit_width));
    c += std::format("            {}.wbin = _wn;\n", st);
    c += "        }\n";
    c += std::format("        {}.rg_s2 = {}.rg_s1; {}.rg_s1 = _rg;\n", st, st, st);
    c += "    }\n";

    c += std::format("    {}.prev_wclk = _wclk; {}.prev_rclk = _rclk;\n", st, st);
    c += "    " + genOutputWrite(0, "_full", 1) + "\n";
    c += "    " + genOutputWrite(1, "_empty", 1) + "\n";
    c += std::format("    uint64_t _q = {}.rdata;\n", st);
    c += "    " + genOutputWrite(2, "_q", _bit_width) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> AsyncFifo::clone(const std::string &n) const {
    return std::make_unique<AsyncFifo>(n, _bit_width, _depth);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"async_fifo", {{"bit_width", "8"}, {"depth", "16"}},
     {"wr_clk", "wr_en", "wr_data", "rd_clk", "rd_en"},
     {"wr_full", "rd_empty", "rd_data"}, dsc::ComponentCategory::Memory,
     "双时钟域异步 FIFO：格雷码指针 + 两级同步器，wr_full/rd_empty 基于同步后的指针"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::AsyncFifo>(n, dsc::paramInt(p, "bit_width", 8), dsc::paramInt(p, "depth", 16));
    });
} // namespace
