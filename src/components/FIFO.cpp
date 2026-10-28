// FIFO — 同步 FIFO 实现
#include "dcs/components/FIFO.h"
#include <format>
#include <stdexcept>

namespace dsc {

FIFO::FIFO(const std::string &name, int data_width, int depth, bool has_rst) :
    SequentialComponent(name, "fifo"), _data_width(data_width), _depth(depth), _has_rst(has_rst) {
    if (data_width != 8 && data_width != 16 && data_width != 32 && data_width != 64)
        throw std::invalid_argument(std::format("数据位宽必须为 8/16/32/64，给定{}", data_width));
    setParam("data_width", std::to_string(data_width));
    setParam("depth", std::to_string(depth));
    setParam("has_rst", has_rst ? "1" : "0");
    if (depth < 2 || depth > 65536 || (depth & (depth - 1)) != 0)
        throw std::invalid_argument("FIFO 深度必须是2的幂（2~65536）");

    _mem.resize(depth * ((data_width + 7) / 8), 0);

    addInput("din", data_width);
    addInput("wr_en", 1);
    addInput("rd_en", 1);
    addInput("clk", 1);
    if (has_rst)
        addInput("rst", 1);
    addOutput("dout", data_width, false, true);
    addOutput("full", 1, false, true);
    addOutput("empty", 1, false, true);
}

std::string FIFO::genStateDecl() const {
    return std::format("static uint8_t* _fifo_p_{0} = (uint8_t*)0x{1:X}ULL;\n"
                       "static int _fifo_wr_ptr_{0} = 0;\n"
                       "static int _fifo_rd_ptr_{0} = 0;\n"
                       "static int _fifo_count_{0} = 0;\n"
                       "static uint8_t _fifo_clk_{0};\n",
                       _id, reinterpret_cast<uintptr_t>(_mem.data()));
}

std::string FIFO::genInitCode() const {
    return std::format("    _fifo_wr_ptr_{0} = 0;\n"
                       "    _fifo_rd_ptr_{0} = 0;\n"
                       "    _fifo_count_{0} = 0;\n"
                       "    _fifo_clk_{0} = 0;",
                       _id);
}

std::string FIFO::genFuncDef_seq() const {
    int id = _id;
    int d_nid = inputs()[0]->netId();
    int d_nw = d_nid >= 0 ? inputs()[0]->net()->bit_width() : 0;
    int we_nid = inputs()[1]->netId();
    int re_nid = inputs()[2]->netId();
    int clk_nid = inputs()[3]->netId();
    int rst_nid = _has_rst ? inputs()[4]->netId() : -1;
    int q_nid = outputs()[0]->netId();
    int f_nid = outputs()[1]->netId();
    int e_nid = outputs()[2]->netId();

    int d_bytes = byte_count(_data_width);
    auto fid = std::to_string(id);
    auto fd = std::to_string(_depth);
    auto fm = std::to_string(_depth - 1);

    std::string code;
    code += "// FIFO: " + std::to_string(_data_width) + "b x " + fd + "\n";
    code += "static void " + funcName_seq() + "(void) {\n";
    code += "    const int d_bytes = " + std::to_string(d_bytes) + ";\n";

    // 读取输入
    code += "    " + gen_read_wire(d_nid, _data_width, d_nw, "_din") + "\n";
    // 悬空引脚必须读常量 0：退回 _w[0] 会读到别的元件的驱动值，把"没接"当成"接了 1"
    auto read_bit = [](const char *name, int nid) {
        return nid >= 0 ? std::format("    bool {} = false; dcs_memcpy(&{}, _w[{}], 1);\n", name, name, nid)
                        : std::format("    bool {} = false;\n", name);
    };
    code += read_bit("_we", we_nid);
    code += read_bit("_re", re_nid);
    code += read_bit("_clk", clk_nid);
    // 引脚存在但悬空时读常量 0，不能退回 _w[0]（那是别的线网）
    if (_has_rst && rst_nid >= 0)
        code += std::format("    bool _rst = false; dcs_memcpy(&_rst, _w[{}], 1);\n", rst_nid);
    else
        code += "    bool _rst = false;\n";

    // 异步复位
    code += "    if (_rst) {\n";
    code += "        _fifo_wr_ptr_" + fid + " = 0;\n";
    code += "        _fifo_rd_ptr_" + fid + " = 0;\n";
    code += "        _fifo_count_" + fid + " = 0;\n";
    code += "    }\n";

    // 时钟上升沿操作
    code += "    if (_clk && !_fifo_clk_" + fid + " && !_rst) {\n";
    // 读操作
    code += "        if (_re && _fifo_count_" + fid + " > 0) {\n";
    code += "            _fifo_rd_ptr_" + fid + " = (_fifo_rd_ptr_" + fid + " + 1) & " + fm + ";\n";
    code += "            _fifo_count_" + fid + "--;\n";
    code += "        }\n";
    // 写操作
    code += "        if (_we && _fifo_count_" + fid + " < " + fd + ") {\n";
    code += "            uint8_t* _wslot = _fifo_p_" + fid + " + _fifo_wr_ptr_" + fid + " * d_bytes;\n";
    code += "            dcs_memcpy(_wslot, &_din, " + std::to_string(d_bytes) + ");\n";
    code += "            _fifo_wr_ptr_" + fid + " = (_fifo_wr_ptr_" + fid + " + 1) & " + fm + ";\n";
    code += "            _fifo_count_" + fid + "++;\n";
    code += "        }\n";
    code += "    }\n";
    code += "    _fifo_clk_" + fid + " = _clk;\n";
    // dout 走 FWFT（first-word fall-through）语义：它任何时刻都等于"下一个可读数据"。
    // 所以输出必须放在 rd_ptr 前进之后再算——读操作那一拍 dout 变成新的 head 是对的，
    // 被读走的数据在 rd_en 之前就已经出现在 dout 上，主设备不会漏读。
    // 这不是"指针先前进导致丢数据"，别照着标准 FIFO 的时序来改。
    if (q_nid >= 0) {
        code += "    uint8_t* _rslot = _fifo_p_" + fid + " + _fifo_rd_ptr_" + fid + " * d_bytes;\n";
        code += "    " + std::string(c_int_type(_data_width)) + " _dout = 0;\n";
        code += "    dcs_memcpy(&_dout, _rslot, " + std::to_string(d_bytes) + ");\n";
        code += "    " + genOutputWrite(0, "_dout", _data_width) + "\n";
    }

    if (f_nid >= 0) {
        code += "    uint8_t _full = (_fifo_count_" + fid + " >= " + fd + ") ? 1 : 0;\n";
        code += "    " + genOutputWrite(1, "_full", 1) + "\n";
    }
    if (e_nid >= 0) {
        code += "    uint8_t _empty = (_fifo_count_" + fid + " == 0) ? 1 : 0;\n";
        code += "    " + genOutputWrite(2, "_empty", 1) + "\n";
    }

    code += "}\n";
    return code;
}

std::unique_ptr<Component> FIFO::clone(const std::string &n) const {
    return std::make_unique<FIFO>(n, _data_width, _depth, _has_rst);
}

} // namespace dsc
