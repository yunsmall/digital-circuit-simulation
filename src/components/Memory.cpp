// Memory — 带读写延迟的可寻址 RAM
#include "dcs/components/Memory.h"
#include <format>
#include <stdexcept>

namespace dsc {

Memory::Memory(const std::string &name, int addr_width, int data_width, int read_latency, int write_latency) :
    SequentialComponent(name, "memory"), _addr_width(addr_width), _data_width(data_width), _depth(1 << addr_width),
    _read_latency(read_latency), _write_latency(write_latency), _rd_stages(read_latency + 1),
    _wr_stages(write_latency + 1) {
    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));
    setParam("read_latency", std::to_string(read_latency));
    setParam("write_latency", std::to_string(write_latency));
    if (addr_width < 1 || addr_width > 12)
        throw std::invalid_argument("地址位宽必须在1-12之间");
    if (data_width < 1 || data_width > 64)
        throw std::invalid_argument("数据位宽必须在1-64之间");
    if (read_latency < 0 || read_latency > 15)
        throw std::invalid_argument("读延迟必须在0-15之间");
    if (write_latency < 0 || write_latency > 15)
        throw std::invalid_argument("写延迟必须在0-15之间");

    _mem.resize(_depth * 16, 0);

    addInput("addr", addr_width);
    addInput("data_in", data_width);
    addInput("we", 1);
    addInput("clk", 1);
    addOutput("data_out", data_width, false, true);
    addOutput("busy", 1, false, true);
}

// ============================================================
// C 代码生成
// ============================================================

std::string Memory::genStateDecl() const {
    int id = _id;
    std::string code;
    code += std::format("static uint8_t* _dcs_mem_p_{0} = (uint8_t*)0x{1:X}ULL;\n", id,
                        reinterpret_cast<uintptr_t>(_mem.data()));
    code += std::format("static uint8_t  _dcs_mem_clk_{0};\n", id);
    // 读流水线
    if (_rd_stages > 0)
        code += std::format("static uint64_t _rd_addr_{0}[{1}];\n", id, _rd_stages);
    // 写流水线
    if (_wr_stages > 0) {
        code += std::format("static uint64_t _wr_addr_{0}[{1}];\n", id, _wr_stages);
        code += std::format("static uint64_t _wr_data_{0}[{1}];\n", id, _wr_stages);
        code += std::format("static uint8_t  _wr_pend_{0}[{1}];\n", id, _wr_stages);
    }
    return code;
}

std::string Memory::genInitCode() const {
    int id = _id;
    std::string code;
    code += std::format("    _dcs_mem_clk_{0} = 0;\n", id);
    code += std::format("    dcs_memset(_rd_addr_{0}, 0, sizeof(_rd_addr_{0}));\n", id);
    if (_wr_stages > 0) {
        code += std::format("    dcs_memset(_wr_addr_{0}, 0, sizeof(_wr_addr_{0}));\n", id);
        code += std::format("    dcs_memset(_wr_data_{0}, 0, sizeof(_wr_data_{0}));\n", id);
        code += std::format("    dcs_memset(_wr_pend_{0}, 0, sizeof(_wr_pend_{0}));\n", id);
    }
    return code;
}

std::string Memory::genFuncDef_seq() const {
    int id = _id;
    int a_nid = inputs()[0]->netId();
    int a_nw = a_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int d_nid = inputs()[1]->netId();
    int d_nw = d_nid >= 0 ? inputs()[1]->net()->bit_width() : 0;
    int we_nid = inputs()[2]->netId();
    int clk_nid = inputs()[3]->netId();
    int q_nid = outputs()[0]->netId();
    int busy_nid = outputs()[1]->netId();
    int d_bytes = byte_count(_data_width);
    std::string addr_mask = gen_mask(_addr_width);

    std::string read_addr = gen_read_wire(a_nid, _addr_width, a_nw, "_addr");
    std::string read_data = gen_read_wire(d_nid, _data_width, d_nw, "_din");
    std::string read_we =
            we_nid >= 0 ? std::format("bool _we = false; dcs_memcpy(&_we, _w[{}], 1);", we_nid) : "bool _we = false;";
    std::string read_clk = clk_nid >= 0 ? std::format("bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);", clk_nid)
                                        : "bool _clk = false;";

    std::string code;
    code += std::format("// Memory: {}×{}, rd_lat={}, wr_lat={}\n", _addr_width, _data_width, _read_latency,
                        _write_latency);
    code += std::format("static void {}() {{\n", funcName_seq());

    // 读取输入
    code += "    " + read_addr + "\n";
    code += "    " + read_data + "\n";
    code += "    " + read_we + "\n";
    code += "    " + read_clk + "\n";
    code += std::format("    _addr &= {};\n", addr_mask);

    // 读流水线：移位
    if (_rd_stages > 1) {
        code += "    // 读流水线移位\n";
        for (int i = _rd_stages - 1; i >= 1; i--)
            code += std::format("    _rd_addr_{0}[{1}] = _rd_addr_{0}[{2}];\n", id, i, i - 1);
    }
    if (_rd_stages > 0)
        code += std::format("    _rd_addr_{0}[0] = _addr;\n", id);

    // 写流水线
    if (_wr_stages > 1) {
        code += "    // 写流水线移位+提交\n";
        int wlast = _wr_stages - 1;
        // 末尾级提交
        code += std::format("    if (_wr_pend_{0}[{1}]) {{\n", id, wlast);
        code += std::format("        uint8_t* _wslot = _dcs_mem_p_{0} + _wr_addr_{0}[{1}] * 16;\n", id, wlast);
        code += std::format("        dcs_memcpy(_wslot, &_wr_data_{0}[{1}], {2});\n", id, wlast, d_bytes);
        code += std::format("        _wr_pend_{0}[{1}] = 0;\n", id, wlast);
        code += "    }\n";
        // 移位
        for (int i = _wr_stages - 1; i >= 1; i--) {
            code += std::format("    _wr_addr_{0}[{1}] = _wr_addr_{0}[{2}];\n", id, i, i - 1);
            code += std::format("    _wr_data_{0}[{1}] = _wr_data_{0}[{2}];\n", id, i, i - 1);
            code += std::format("    _wr_pend_{0}[{1}] = _wr_pend_{0}[{2}];\n", id, i, i - 1);
        }
        code += std::format("    _wr_pend_{0}[0] = 0;\n", id);
        // 捕获
        code += std::format("    if (_clk && !_dcs_mem_clk_{0} && _we) {{\n", id);
        code += std::format("        _wr_addr_{0}[0] = _addr;\n", id);
        code += std::format("        _wr_data_{0}[0] = _din & 0x{1:X}ULL;\n", id,
                            (_data_width == 64) ? ~0ULL : (1ULL << _data_width) - 1);
        code += std::format("        _wr_pend_{0}[0] = 1;\n", id);
        code += "    }\n";
    }
    else {
        // 零延迟：上升沿直接写入
        code += "    // 零写延迟：直接写入\n";
        code += std::format("    if (_clk && !_dcs_mem_clk_{0} && _we) {{\n", id);
        code += std::format("        uint8_t* _wslot = _dcs_mem_p_{0} + _addr * 16;\n", id);
        code += std::format("        uint64_t _masked = _din & 0x{0:X}ULL;\n",
                            (_data_width == 64) ? ~0ULL : (1ULL << _data_width) - 1);
        code += std::format("        dcs_memcpy(_wslot, &_masked, {});\n", d_bytes);
        code += "    }\n";
    }

    code += std::format("    _dcs_mem_clk_{0} = _clk;\n", id);

    // 读输出：从读流水线末尾取值
    if (q_nid >= 0) {
        int rlast = _rd_stages - 1;
        std::string d_type = c_int_type(_data_width);
        if (_rd_stages > 0) {
            code += std::format("    uint8_t* _rslot = _dcs_mem_p_{0} + _rd_addr_{0}[{1}] * 16;\n", id, rlast);
            code += std::format("    {} _dout = 0;\n", d_type);
            code += std::format("    dcs_memcpy(&_dout, _rslot, {});\n", d_bytes);
        }
        else {
            // 0 延迟：直接从当前地址读
            code += std::format("    uint8_t* _rslot = _dcs_mem_p_{0} + _addr * 16;\n", id);
            code += std::format("    {} _dout = 0;\n", d_type);
            code += std::format("    dcs_memcpy(&_dout, _rslot, {});\n", d_bytes);
        }
        code += "    " + genOutputWrite(0, "_dout", _data_width) + "\n";
    }

    // busy 输出
    if (busy_nid >= 0) {
        if (_wr_stages > 0) {
            code += "    uint8_t _busy = 0;\n";
            for (int i = 0; i < _wr_stages; i++)
                code += std::format("    if (_wr_pend_{0}[{1}]) _busy = 1;\n", id, i);
        }
        else {
            code += "    uint8_t _busy = 0;\n";
        }
        code += std::format("    {}\n", genOutputWrite(1, "_busy", 1));
    }

    code += "}\n";
    return code;
}

std::unique_ptr<Component> Memory::clone(const std::string &n) const {
    return std::make_unique<Memory>(n, _addr_width, _data_width, _read_latency, _write_latency);
}


} // namespace dsc
