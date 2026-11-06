// Memory — 带读写延迟的可寻址 RAM（字节寻址）
#include "dcs/ComponentFactory.h"
#include "dcs/components/memory/Memory.h"
#include "dcs/util/MemoryDepth.h"
#include <format>
#include <stdexcept>

namespace dsc {

Memory::Memory(const std::string &name, int addr_width, int data_width, int read_latency, int write_latency) :
    SequentialComponent(name, "memory"), _addr_width(addr_width), _data_width(data_width), _depth(depthFromAddrWidth(addr_width)),
    _read_latency(read_latency), _write_latency(write_latency), _rd_stages(read_latency + 1),
    _wr_stages(write_latency + 1) {
    if (data_width != 8 && data_width != 16 && data_width != 32 && data_width != 64)
        throw std::invalid_argument(std::format("数据位宽必须为 8/16/32/64，给定{}", data_width));
    // 延迟直接是生成代码里静态数组的维度，不设上限时一个笔误就是几十万行
    if (read_latency < 0 || read_latency > 1024 || write_latency < 0 || write_latency > 1024)
        throw std::invalid_argument("读写延迟必须在0-1024之间");

    setParam("addr_width", std::to_string(addr_width));
    setParam("data_width", std::to_string(data_width));
    setParam("read_latency", std::to_string(read_latency));
    setParam("write_latency", std::to_string(write_latency));

    addInput("addr", addr_width);
    addInput("data_in", data_width);
    addInput("we", 1);
    addInput("clk", 1, PinEdge::Rising);
    addOutput("data_out", data_width, false, true);
    addOutput("busy", 1, false, true);
}

bool Memory::prepareImpl(Context &, std::string &) {
    allocateMem();
    return true;
}

void Memory::allocateMem() {
    if (_mem.size() != (size_t) _depth)
        _mem.assign((size_t) _depth, 0);
}

// ============================================================
// C 代码生成
// ============================================================

std::string Memory::genStateDecl() const {
    int id = _id;
    std::string code;
    code += std::format("static uint8_t* _dcs_mem_p_{0} = (uint8_t*)0x{1:X}ULL;\n", id,
                        reinterpret_cast<uintptr_t>(_mem.data()));
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
    int q_nid = outputs()[0]->netId();
    int busy_nid = outputs()[1]->netId();
    int d_bytes = byte_count(_data_width);
    std::string addr_mask = gen_mask(_addr_width);
    auto sid = std::to_string(id);

    std::string read_addr = gen_read_wire(a_nid, _addr_width, a_nw, "_addr");
    std::string read_data = gen_read_wire(d_nid, _data_width, d_nw, "_din");
    std::string read_we =
            we_nid >= 0 ? std::format("bool _we = false; dcs_memcpy(&_we, _w[{}], 1);", we_nid) : "bool _we = false;";

    std::string cap_s = std::to_string(_depth); // 容量（字节数）

    std::string code;
    code += "// Memory: " + std::to_string(_data_width) + "b × " + cap_s + "B, rd_lat=" +
            std::to_string(_read_latency) + ", wr_lat=" + std::to_string(_write_latency) + "\n";
    code += std::format("static void {}() {{\n", funcName_seq());
    code += std::format("    const int d_bytes = {};\n", d_bytes);

    // 读取输入
    code += "    " + read_addr + "\n";
    code += "    " + read_data + "\n";
    code += "    " + read_we + "\n";
    code += std::format("    _addr &= {};\n", addr_mask);

    // 读流水线：移位
    if (_rd_stages > 1) {
        for (int i = _rd_stages - 1; i >= 1; i--)
            code += std::format("    _rd_addr_{0}[{1}] = _rd_addr_{0}[{2}];\n", id, i, i - 1);
    }
    if (_rd_stages > 0)
        code += std::format("    _rd_addr_{0}[0] = _addr;\n", id);

    // 读输出放在写之前：同一拍对同一地址读写时返回写之前的旧值，和
    // MultiPortRAM / RegFile（读优先）保持一致。否则换一种存储器实现，
    // 同拍读写的语义会静默翻转
    if (q_nid >= 0) {
        int rlast = _rd_stages - 1;
        std::string d_type = c_int_type(_data_width);
        std::string ra = _rd_stages > 0
            ? std::format("_rd_addr_{}[{}]", id, rlast)
            : "_addr";
        code += std::format("    {} _dout = 0;\n", d_type);
        code += "    if ((uint64_t)" + ra + " + " + std::to_string(d_bytes) + " <= " + cap_s + "ULL) {\n";
        code += std::format("        dcs_memcpy(&_dout, _dcs_mem_p_{} + {}, {});\n", id, ra, d_bytes);
        code += "    }\n";
        code += "    " + genOutputWrite(0, "_dout", _data_width) + "\n";
    }

    // 写流水线
    if (_wr_stages > 1) {
        code += "    // 写流水线移位+提交\n";
        int wlast = _wr_stages - 1;
        // 顺序是"采样 → 提交 → 移位"：提交处理的是上一拍移到末级的请求，
        // 此拍采样的还要走 _wr_stages-1 次移位，总延迟才是 write_latency
        code += std::format("    _wr_pend_{0}[0] = 0;\n", id);
        code += "    if (" + genGetWireEdge(3, PinEdge::Rising) + " && _we) {\n";
        code += std::format("        _wr_addr_{0}[0] = _addr;\n", id);
        code += std::format("        _wr_data_{0}[0] = _din & 0x{1:X}ULL;\n", id,
                            (_data_width == 64) ? ~0ULL : (1ULL << _data_width) - 1);
        code += std::format("        _wr_pend_{0}[0] = 1;\n", id);
        code += "    }\n";
        code += std::format("    if (_wr_pend_{0}[{1}]) {{\n", id, wlast);
        code += "        if ((uint64_t)_wr_addr_" + sid + "[" + std::to_string(wlast) + "] + " +
                std::to_string(d_bytes) + " <= " + cap_s + "ULL) {\n";
        code += std::format("            dcs_memcpy(_dcs_mem_p_{0} + _wr_addr_{0}[{1}], &_wr_data_{0}[{1}], {2});\n",
                            id, wlast, d_bytes);
        code += "        }\n";
        code += std::format("        _wr_pend_{0}[{1}] = 0;\n", id, wlast);
        code += "    }\n";
        // 移位
        for (int i = _wr_stages - 1; i >= 1; i--) {
            code += std::format("    _wr_addr_{0}[{1}] = _wr_addr_{0}[{2}];\n", id, i, i - 1);
            code += std::format("    _wr_data_{0}[{1}] = _wr_data_{0}[{2}];\n", id, i, i - 1);
            code += std::format("    _wr_pend_{0}[{1}] = _wr_pend_{0}[{2}];\n", id, i, i - 1);
        }
    }
    else {
        code += "    // 零写延迟：直接写入\n";
        code += "    if (" + genGetWireEdge(3, PinEdge::Rising) + " && _we) {\n";
        code += std::format("        uint64_t _masked = _din & 0x{0:X}ULL;\n",
                            (_data_width == 64) ? ~0ULL : (1ULL << _data_width) - 1);
        code += "        if ((uint64_t)_addr + " + std::to_string(d_bytes) + " <= " + cap_s + "ULL) {\n";
        code += "            dcs_memcpy(_dcs_mem_p_" + sid + " + _addr, &_masked, " + std::to_string(d_bytes) + ");\n";
        code += "        }\n";
        code += "    }\n";
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
    auto m = std::make_unique<Memory>(n, _addr_width, _data_width, _read_latency, _write_latency);
    // clone 要连存储内容一起复制，和 ROM/WishboneMemory/VirtioBlkMem 保持一致；
    // allocateMem 容量一致时不清零，prepare 不会把这里的数据清掉
    m->_mem = _mem;
    return m;
}


} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"memory",
                        {{"addr_width", "4"}, {"data_width", "8", {"8", "16", "32", "64"}}, {"read_latency", "0"}, {"write_latency", "0"}},
                        {"addr", "data_in", "we", "clk"},
                        {"data_out", "busy"},
                        dsc::ComponentCategory::Memory, "可寻址RAM，支持独立配置读写延迟"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
                           return std::make_unique<dsc::Memory>(n, dsc::paramInt(p, "addr_width", 4), dsc::paramInt(p, "data_width", 8),
                                                                dsc::paramInt(p, "read_latency", 0), dsc::paramInt(p, "write_latency", 0));
                       });
} // namespace
