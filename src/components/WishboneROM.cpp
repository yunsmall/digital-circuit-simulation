// WishboneROMAdapter — ROM 的 Wishbone 从设备适配器实现
//
// 通过网表连接 ROM：
//   适配器 rom_addr 输出 → ROM addr 输入
//   ROM data_out 输出   → 适配器 rom_data 输入
//
// 使用 genOutputWrite 写 _c_out_（标准路径），gen_read_wire 读 _w[]。
// circuit_tick 每 tick 先复制 _c_out_ → _w[]，再执行 seq 函数。
// 因此 rom_addr 在 tick N 写入 _c_out_，tick N+1 才出现在 _w[] 被 ROM 读取。
// 共需 wait = read_latency + 2 拍后数据就绪。
#include "dcs/components/WishboneROM.h"
#include <cstring>
#include <format>

namespace dsc {

WishboneROMAdapter::WishboneROMAdapter(const std::string &name, int addr_width, int data_width,
                                       int read_latency)
    : SequentialComponent(name, "wishbone_rom_adapter")
    , _addr_width(addr_width)
    , _data_width(data_width)
    , _read_latency(read_latency)
    , _d_bytes(data_width / 8)
{
    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));
    setParam("read_latency", std::to_string(read_latency));

    addInput("wb_adr_i", addr_width);
    addInput("wb_dat_i", data_width);
    addInput("wb_we_i", 1);
    addInput("wb_sel_i", _d_bytes);
    addInput("wb_stb_i", 1);
    addInput("wb_cyc_i", 1);
    addInput("clk", 1);
    addInput("rom_data", data_width);
    addOutput("rom_addr", addr_width);
    addOutput("wb_dat_o", data_width, false, true);
    addOutput("wb_ack_o", 1, false, true);
}

std::string WishboneROMAdapter::genStateDecl() const {
    int id = _id;
    std::string code;
    code += std::format("static bool    _bra_prev_{0};\n", id);
    code += std::format("static int     _bra_wait_{0};\n", id);
    code += std::format("static uint64_t _bra_adr_{0};\n", id);
    code += std::format("static bool    _bra_ack_{0};\n", id);
    code += std::format("static {1} _bra_dat_{0};\n", id, c_int_type(_data_width));
    return code;
}

std::string WishboneROMAdapter::genInitCode() const {
    int id = _id;
    std::string code;
    code += std::format("    _bra_prev_{0} = false;\n", id);
    code += std::format("    _bra_wait_{0} = -1;\n", id);
    code += std::format("    _bra_adr_{0} = 0;\n", id);
    code += std::format("    _bra_ack_{0} = false;\n", id);
    code += std::format("    _bra_dat_{0} = 0;\n", id);
    return code;
}

std::string WishboneROMAdapter::genFuncDef_seq() const {
    int id = _id;

    int adr_in   = inputs()[0]->netId();  // wb_adr_i
    int stb_in   = inputs()[4]->netId();  // wb_stb_i
    int rdat_in  = inputs()[7]->netId();  // rom_data

    int adr_nw = adr_in >= 0 ? inputs()[0]->net()->bit_width() : 0;
    std::string adr_mask = gen_mask(_addr_width);
    std::string d_type = c_int_type(_data_width);

    std::string code;
    code += std::format("// WishboneROMAdapter id={}\n", id);
    code += std::format("static void {}() {{\n", funcName_seq());

    // 读 adr / stb
    if (adr_in >= 0) {
        code += "    " + gen_read_wire(adr_in, _addr_width, adr_nw, "_adr") + "\n";
        code += std::format("    _adr &= {};\n", adr_mask);
    } else {
        code += "    uint64_t _adr = 0;\n";
    }
    if (stb_in >= 0) {
        code += std::format("    bool _stb = false; dcs_memcpy(&_stb, _w[{}], 1);\n", stb_in);
    } else {
        code += "    bool _stb = false;\n";
    }

    // 先递减（在 rise 检测之前，避免同一 tick 刚设就减）
    code += std::format("    if (_bra_wait_{0} > 0) _bra_wait_{0}--;\n", id);

    // stb 上升沿 → 新事务
    // wait = read_latency + 2：
    //   +1: ROM 在适配器之前运行，下一拍才读到 rom_addr
    //   +1: ROM 输出经 _c_out_ → _w[] 复制，再下一拍适配器才读到 rom_data
    code += std::format("    bool _rise = _stb && !_bra_prev_{0};\n", id);
    code += std::format("    _bra_prev_{0} = _stb;\n", id);
    code += std::format("    if (_rise && _bra_wait_{0} < 0) {{\n", id);
    code += std::format("        _bra_adr_{0} = _adr;\n", id);
    code += std::format("        _bra_wait_{0} = {1} + 2;\n", id, _read_latency);
    code += std::format("        _bra_ack_{0} = false;\n", id);
    code += "    }\n";

    // 驱动 rom_addr（通过 genOutputWrite → _c_out_，避免被 circuit_tick 覆盖）
    code += "    " + genOutputWrite(0, std::format("_bra_adr_{}", id), _addr_width) + "\n";

    // wait == 0 → 读 rom_data，断言 ack
    code += std::format("    if (_bra_wait_{0} == 0) {{\n", id);
    if (rdat_in >= 0) {
        code += std::format("        dcs_memcpy(&_bra_dat_{0}, _w[{1}], {2});\n",
                          id, rdat_in, _d_bytes);
    }
    code += std::format("        _bra_ack_{0} = true;\n", id);
    code += "    }\n";

    // stb 撤销 → 事务结束
    code += std::format("    if (!_stb && _bra_ack_{0}) {{\n", id);
    code += std::format("        _bra_ack_{0} = false;\n", id);
    code += std::format("        _bra_wait_{0} = -1;\n", id);
    code += "    }\n";

    // 写 Wishbone 输出（使用标准 _c_out_ 路径）
    code += "    " + genOutputWrite(2, std::format("_bra_ack_{}", id), 1) + "\n";
    code += "    " + genOutputWrite(1, std::format("_bra_dat_{}", id), _data_width) + "\n";

    code += "}\n";
    return code;
}

std::unique_ptr<Component> WishboneROMAdapter::clone(const std::string &n) const {
    return std::make_unique<WishboneROMAdapter>(n, _addr_width, _data_width, _read_latency);
}

} // namespace dsc
