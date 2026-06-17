// MultiPortRAM — 参数化多端口 RAM 实现
#include "dcs/components/MultiPortRAM.h"
#include <format>
#include <stdexcept>

namespace dsc {

MultiPortRAM::MultiPortRAM(const std::string &name, int addr_width, int data_width, int num_read_ports,
                           int num_write_ports, int read_latency) :
    SequentialComponent(name, "mpram"), _addr_width(addr_width), _data_width(data_width), _num_read(num_read_ports),
    _num_write(num_write_ports), _read_latency(read_latency), _rd_stages(read_latency), _depth(1 << addr_width) {

    if (addr_width < 1 || addr_width > 12)
        throw std::invalid_argument("地址位宽必须在1-12之间");
    if (data_width < 1 || data_width > 64)
        throw std::invalid_argument("数据位宽必须在1-64之间");
    if (num_read_ports < 1 || num_read_ports > 4)
        throw std::invalid_argument("读端口数必须在1-4之间");
    if (num_write_ports < 1 || num_write_ports > 2)
        throw std::invalid_argument("写端口数必须在1-2之间");
    if (read_latency < 0 || read_latency > 15)
        throw std::invalid_argument("读延迟必须在0-15之间");

    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));
    setParam("num_read_ports", std::to_string(num_read_ports));
    setParam("num_write_ports", std::to_string(num_write_ports));
    setParam("read_latency", std::to_string(read_latency));

    _mem.resize(_depth * 16, 0);

    // 时钟
    addInput("clk", 1);

    // 写端口引脚
    for (int i = 0; i < _num_write; i++) {
        addInput(std::format("wr_addr_{}", i), addr_width);
        addInput(std::format("wr_data_{}", i), data_width);
        addInput(std::format("we_{}", i), 1);
    }

    // 读端口引脚
    for (int i = 0; i < _num_read; i++) {
        addInput(std::format("rd_addr_{}", i), addr_width);
        addOutput(std::format("rd_data_{}", i), data_width, false, true);
    }

    // 流控：读数据有效标志（read_latency=0 时始终为 1）
    addOutput("rd_valid", 1, false, true);
}

std::string MultiPortRAM::genStateDecl() const {
    int id = _id;
    std::string code;
    code += std::format("static uint8_t* _mpram_mem_{0} = (uint8_t*)0x{1:X}ULL;\n", id,
                        reinterpret_cast<uintptr_t>(_mem.data()));
    code += std::format("static uint8_t  _mpram_clk_{0};\n", id);

    if (_rd_stages > 0) {
        code += std::format("static uint64_t _mpram_rd_addr_{0}[{1}][{2}];\n", id, _rd_stages, _num_read);
        code += std::format("static int _mpram_tick_{0};\n", id);
    }
    return code;
}

std::string MultiPortRAM::genInitCode() const {
    int id = _id;
    std::string code;
    code += std::format("    _mpram_clk_{0} = 0;\n", id);
    if (_rd_stages > 0) {
        code += std::format("    dcs_memset(_mpram_rd_addr_{0}, 0, sizeof(_mpram_rd_addr_{0}));\n", id);
        code += std::format("    _mpram_tick_{0} = 0;\n", id);
    }
    return code;
}

std::string MultiPortRAM::genFuncDef_seq() const {
    int id = _id;
    int clk_nid = inputs()[0]->netId();
    int d_bytes = byte_count(_data_width);
    std::string addr_mask = gen_mask(_addr_width);
    auto sid = std::to_string(id);

    // 写端口 net ID
    std::vector<int> wa_nid(_num_write), wd_nid(_num_write), we_nid(_num_write);
    for (int i = 0; i < _num_write; i++) {
        int base = 1 + i * 3;
        wa_nid[i] = inputs()[base]->netId();
        wd_nid[i] = inputs()[base + 1]->netId();
        we_nid[i] = inputs()[base + 2]->netId();
    }

    // 读端口 net ID
    std::vector<int> ra_nid(_num_read);
    for (int i = 0; i < _num_read; i++) {
        int base = 1 + _num_write * 3 + i;
        ra_nid[i] = inputs()[base]->netId();
    }

    // 输出 net ID: rd_data_0..rd_data_{N-1} 然后 rd_valid
    std::vector<int> q_nid(_num_read);
    for (int i = 0; i < _num_read; i++)
        q_nid[i] = outputs()[i]->netId();
    int valid_nid = outputs()[_num_read]->netId();

    std::string code;
    code += "// MultiPortRAM: " + std::to_string(_data_width) + "b x " + std::to_string(_depth) + ", " +
            std::to_string(_num_read) + "R/" + std::to_string(_num_write) +
            "W, rd_lat=" + std::to_string(_read_latency) + "\n";
    code += "static void " + funcName_seq() + "(void) {\n";

    // ---- 第1步：读取所有输入 ----
    code += "    " + gen_read_wire(clk_nid, 1, 1, "_clk") + "\n";
    for (int i = 0; i < _num_write; i++) {
        int wa_nw = wa_nid[i] >= 0 ? inputs()[1 + i * 3]->net()->bit_width() : 0;
        int wd_nw = wd_nid[i] >= 0 ? inputs()[1 + i * 3 + 1]->net()->bit_width() : 0;
        code += "    " + gen_read_wire(wa_nid[i], _addr_width, wa_nw, std::format("_wa{}", i)) + "\n";
        code += "    " + gen_read_wire(wd_nid[i], _data_width, wd_nw, std::format("_wd{}", i)) + "\n";
        code += std::format("    bool _we{} = false; dcs_memcpy(&_we{}, _w[{}], 1);\n", i, i,
                            we_nid[i] >= 0 ? we_nid[i] : 0);
        code += std::format("    _wa{} &= {};\n", i, addr_mask);
    }
    for (int i = 0; i < _num_read; i++) {
        int ra_nw = ra_nid[i] >= 0 ? inputs()[1 + _num_write * 3 + i]->net()->bit_width() : 0;
        code += "    " + gen_read_wire(ra_nid[i], _addr_width, ra_nw, std::format("_ra{}", i)) + "\n";
        code += std::format("    _ra{} &= {};\n", i, addr_mask);
    }

    // ---- 第2步：输出读数据和 valid 标志 ----
    for (int i = 0; i < _num_read; i++) {
        if (q_nid[i] < 0)
            continue;
        if (_rd_stages > 0) {
            int rlast = _rd_stages - 1;
            code += std::format("    uint8_t* _rslot{} = _mpram_mem_{} + _mpram_rd_addr_{}[{}][{}] * 16;\n", i, sid,
                                sid, rlast, i);
        }
        else {
            code += std::format("    uint8_t* _rslot{} = _mpram_mem_{} + _ra{} * 16;\n", i, sid, i);
        }
        code += std::format("    {} _dout{} = 0;\n", c_int_type(_data_width), i);
        code += std::format("    dcs_memcpy(&_dout{}, _rslot{}, {});\n", i, i, d_bytes);
        code += "    " + genOutputWrite(i, std::format("_dout{}", i), _data_width) + "\n";
    }

    // rd_valid: read_latency==0 时始终为 1，否则等流水线填满（_mpram_tick >= read_latency）
    if (valid_nid >= 0) {
        if (_rd_stages > 0)
            code += std::format("    uint8_t _rd_valid = (_mpram_tick_{0} > {1}) ? 1 : 0;\n", sid, _read_latency);
        else
            code += "    uint8_t _rd_valid = 1;\n";
        code += "    " + genOutputWrite(_num_read, "_rd_valid", 1) + "\n";
    }

    // ---- 第3步：写端口（时钟上升沿，we=1 时写入）----
    if (_num_write > 0) {
        code += std::format("    if (_clk && !_mpram_clk_{}) {{\n", sid);
        for (int i = 0; i < _num_write; i++) {
            code += std::format("        if (_we{}) {{\n", i);
            code += std::format("            uint8_t* _wslot{} = _mpram_mem_{} + _wa{} * 16;\n", i, sid, i);
            code += std::format("            dcs_memcpy(_wslot{}, &_wd{}, {});\n", i, i, d_bytes);
            code += std::format("            dcs_memset(_wslot{} + {}, 0, 16 - {});\n", i, d_bytes, d_bytes);
            code += "        }\n";
        }
        code += "    }\n";
    }

    // ---- 第4步：读流水线（每 tick 无条件移位+捕获）----
    if (_rd_stages > 1) {
        for (int s = _rd_stages - 1; s >= 1; s--)
            for (int i = 0; i < _num_read; i++)
                code += std::format("    _mpram_rd_addr_{0}[{1}][{2}] = _mpram_rd_addr_{0}[{3}][{2}];\n", sid, s, i,
                                    s - 1);
    }
    if (_rd_stages > 0) {
        for (int i = 0; i < _num_read; i++)
            code += std::format("    _mpram_rd_addr_{0}[0][{1}] = _ra{1};\n", sid, i);
        code += std::format("    _mpram_tick_{0}++;\n", sid);
    }

    code += std::format("    _mpram_clk_{} = _clk;\n", sid);
    code += "}\n";
    return code;
}

std::unique_ptr<Component> MultiPortRAM::clone(const std::string &n) const {
    return std::make_unique<MultiPortRAM>(n, _addr_width, _data_width, _num_read, _num_write, _read_latency);
}

} // namespace dsc
