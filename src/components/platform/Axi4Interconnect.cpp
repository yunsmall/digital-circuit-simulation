// Axi4Interconnect 实现：地址译码 + ID 扩展/回送 + 写/读两条独立转发状态机。
#include "dcs/components/platform/Axi4Interconnect.h"

#include <format>
#include <stdexcept>

#include "dcs/ComponentFactory.h"

namespace dsc {

namespace {
// 主口输入字段顺序（每主口 18 个）
enum MIn { MI_AWID = 0, MI_AWADDR, MI_AWLEN, MI_AWSIZE, MI_AWBURST, MI_AWVALID,
           MI_WDATA, MI_WSTRB, MI_WLAST, MI_WVALID, MI_BREADY,
           MI_ARID, MI_ARADDR, MI_ARLEN, MI_ARSIZE, MI_ARBURST, MI_ARVALID, MI_RREADY, MI_COUNT };
// 主口输出字段顺序（每主口 11 个）
enum MOut { MO_AWREADY = 0, MO_WREADY, MO_BID, MO_BRESP, MO_BVALID,
            MO_ARREADY, MO_RID, MO_RDATA, MO_RRESP, MO_RLAST, MO_RVALID, MO_COUNT };
// 从口输入字段顺序（每从口 11 个）
enum SIn { SI_AWREADY = 0, SI_WREADY, SI_BID, SI_BRESP, SI_BVALID,
           SI_ARREADY, SI_RID, SI_RDATA, SI_RRESP, SI_RLAST, SI_RVALID, SI_COUNT };
// 从口输出字段顺序（每从口 18 个）
enum SOut { SO_AWID = 0, SO_AWADDR, SO_AWLEN, SO_AWSIZE, SO_AWBURST, SO_AWVALID,
            SO_WDATA, SO_WSTRB, SO_WLAST, SO_WVALID, SO_BREADY,
            SO_ARID, SO_ARADDR, SO_ARLEN, SO_ARSIZE, SO_ARBURST, SO_ARVALID, SO_RREADY, SO_COUNT };
} // namespace

Axi4Interconnect::Axi4Interconnect(const std::string &name, const std::string &segments, int num_masters,
                                   int id_width)
    : SequentialComponent(name, "axi4_ic"), _num_masters(num_masters), _id_width(id_width) {
    if (_num_masters < 1)
        throw std::invalid_argument("axi4_ic: num_masters 必须 >= 1");
    if (_id_width < 1 || _id_width > 8)
        throw std::invalid_argument("axi4_ic: id_width 必须在 1..8");
    // 解析地址段，格式与 wishbone_ic 一致
    size_t pos = 0;
    while (pos < segments.size()) {
        const size_t comma = segments.find(',', pos);
        const std::string item = segments.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t colon = item.find(':');
        if (colon == std::string::npos)
            throw std::invalid_argument("axi4_ic: segments 每项必须是 base:size");
        Segment seg{std::stoull(item.substr(0, colon)), std::stoull(item.substr(colon + 1))};
        if (seg.size == 0)
            throw std::invalid_argument("axi4_ic: 地址段大小不能为 0");
        _segments.push_back(seg);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (_segments.empty())
        throw std::invalid_argument("axi4_ic: 至少需要一个地址段");
    _extra_bits = 0;
    while ((1u << _extra_bits) < (unsigned) _num_masters) _extra_bits++;
    _slave_id_width = _id_width + _extra_bits;

    setParam("segments", segments);
    setParam("num_masters", std::to_string(num_masters));
    setParam("id_width", std::to_string(id_width));

    addInput("clk", 1, PinEdge::Rising);
    addInput("rst", 1);
    for (int m = 0; m < _num_masters; m++) {
        const std::string p = "s" + std::to_string(m) + "_axi_";
        addInput(p + "awid", _id_width);
        addInput(p + "awaddr", 32);
        addInput(p + "awlen", 8);
        addInput(p + "awsize", 3);
        addInput(p + "awburst", 2);
        addInput(p + "awvalid", 1);
        addInput(p + "wdata", 32);
        addInput(p + "wstrb", 4);
        addInput(p + "wlast", 1);
        addInput(p + "wvalid", 1);
        addInput(p + "bready", 1);
        addInput(p + "arid", _id_width);
        addInput(p + "araddr", 32);
        addInput(p + "arlen", 8);
        addInput(p + "arsize", 3);
        addInput(p + "arburst", 2);
        addInput(p + "arvalid", 1);
        addInput(p + "rready", 1);
    }
    for (size_t s = 0; s < _segments.size(); s++) {
        const std::string p = "m" + std::to_string(s) + "_axi_";
        addInput(p + "awready", 1);
        addInput(p + "wready", 1);
        addInput(p + "bid", _slave_id_width);
        addInput(p + "bresp", 2);
        addInput(p + "bvalid", 1);
        addInput(p + "arready", 1);
        addInput(p + "rid", _slave_id_width);
        addInput(p + "rdata", 32);
        addInput(p + "rresp", 2);
        addInput(p + "rlast", 1);
        addInput(p + "rvalid", 1);
    }
    for (int m = 0; m < _num_masters; m++) {
        const std::string p = "s" + std::to_string(m) + "_axi_";
        addOutput(p + "awready", 1);
        addOutput(p + "wready", 1);
        addOutput(p + "bid", _id_width);
        addOutput(p + "bresp", 2);
        addOutput(p + "bvalid", 1);
        addOutput(p + "arready", 1);
        addOutput(p + "rid", _id_width);
        addOutput(p + "rdata", 32);
        addOutput(p + "rresp", 2);
        addOutput(p + "rlast", 1);
        addOutput(p + "rvalid", 1);
    }
    for (size_t s = 0; s < _segments.size(); s++) {
        const std::string p = "m" + std::to_string(s) + "_axi_";
        addOutput(p + "awid", _slave_id_width);
        addOutput(p + "awaddr", 32);
        addOutput(p + "awlen", 8);
        addOutput(p + "awsize", 3);
        addOutput(p + "awburst", 2);
        addOutput(p + "awvalid", 1);
        addOutput(p + "wdata", 32);
        addOutput(p + "wstrb", 4);
        addOutput(p + "wlast", 1);
        addOutput(p + "wvalid", 1);
        addOutput(p + "bready", 1);
        addOutput(p + "arid", _slave_id_width);
        addOutput(p + "araddr", 32);
        addOutput(p + "arlen", 8);
        addOutput(p + "arsize", 3);
        addOutput(p + "arburst", 2);
        addOutput(p + "arvalid", 1);
        addOutput(p + "rready", 1);
    }
}

Axi4Interconnect::~Axi4Interconnect() = default;

int Axi4Interconnect::masterInIdx(int m, int which) const { return 2 + m * MI_COUNT + which; }
int Axi4Interconnect::masterOutIdx(int m, int which) const { return m * MO_COUNT + which; }
int Axi4Interconnect::slaveInIdx(int s, int which) const { return 2 + _num_masters * MI_COUNT + s * SI_COUNT + which; }
int Axi4Interconnect::slaveOutIdx(int s, int which) const {
    return _num_masters * MO_COUNT + s * SO_COUNT + which;
}

std::string Axi4Interconnect::genDecode(const std::string &addr_var, int out_var) const {
    std::string c = std::format("        int _seg{} = -1;\n", out_var);
    for (size_t s = 0; s < _segments.size(); s++) {
        c += std::format("        if (_seg{0} < 0 && (unsigned long long) {1} >= {2}ull && "
                         "(unsigned long long) {1} - {2}ull < {3}ull) _seg{0} = {4};\n",
                         out_var, addr_var, _segments[s].base, _segments[s].size, (int) s);
    }
    return c;
}

std::string Axi4Interconnect::genStateDecl() const {
    return std::format(
            "static uint8_t _ax4ic_wst_{0}, _ax4ic_rst2_{0};\n"
            "static int _ax4ic_wm_{0}, _ax4ic_ws_{0}, _ax4ic_rm_{0}, _ax4ic_rs_{0};\n"
            "static uint32_t _ax4ic_waddr_{0}, _ax4ic_raddr_{0};\n"
            "static uint8_t _ax4ic_wlen_{0}, _ax4ic_wsize_{0}, _ax4ic_wburst_{0};\n"
            "static uint8_t _ax4ic_rlen_{0}, _ax4ic_rsize_{0}, _ax4ic_rburst_{0};\n"
            "static uint16_t _ax4ic_wid_{0}, _ax4ic_rid_{0}, _ax4ic_sid_{0};\n"
            "static uint8_t _ax4ic_wbuf_{0}[16], _ax4ic_rbuf_{0}[16];\n"
            "static uint8_t _ax4ic_wbuf_valid_{0}, _ax4ic_wbuf_strb_{0}, _ax4ic_wbuf_last_{0};\n"
            "static uint8_t _ax4ic_rbuf_valid_{0}, _ax4ic_rbuf_resp_{0}, _ax4ic_rbuf_last_{0};\n"
            "static uint16_t _ax4ic_rbuf_rid_{0};\n"
            "static uint8_t _ax4ic_bvalid_{0}, _ax4ic_bdone_{0}, _ax4ic_bresp_{0};\n"
            "static uint16_t _ax4ic_bid_{0};\n"
            "static uint8_t _ax4ic_rdone_{0};\n"
            // 响应已从从口接收、等待送给主口。和 VALID 分开是因为同拍"锁存即送出"
            // 会让主口在撤 READY 后立刻看到 VALID 被撤销，握手被吞掉
            "static uint8_t _ax4ic_b_got_{0}, _ax4ic_r_got_{0};\n"
            // 已经向主口送出过 VALID，下一拍才检查 READY（主口采样 VALID 的那一拍
            // 不能同时撤销 VALID）
            "static uint8_t _ax4ic_b_sent_{0}, _ax4ic_r_sent_{0};\n"
            "static uint32_t _ax4ic_dec_left_{0};\n",
            _id);
}

std::string Axi4Interconnect::genInitCode() const {
    const int id = _id;
    return std::format(
            "    _ax4ic_wst_{0} = 0; _ax4ic_rst2_{0} = 0;\n"
            "    _ax4ic_wm_{0} = 0; _ax4ic_ws_{0} = -1; _ax4ic_rm_{0} = 0; _ax4ic_rs_{0} = -1;\n"
            "    _ax4ic_waddr_{0} = 0; _ax4ic_raddr_{0} = 0;\n"
            "    _ax4ic_wlen_{0} = 0; _ax4ic_wsize_{0} = 0; _ax4ic_wburst_{0} = 1;\n"
            "    _ax4ic_rlen_{0} = 0; _ax4ic_rsize_{0} = 0; _ax4ic_rburst_{0} = 1;\n"
            "    _ax4ic_wid_{0} = 0; _ax4ic_rid_{0} = 0; _ax4ic_sid_{0} = 0;\n"
            "    dcs_memset(_ax4ic_wbuf_{0}, 0, 16); dcs_memset(_ax4ic_rbuf_{0}, 0, 16);\n"
            "    _ax4ic_wbuf_valid_{0} = 0; _ax4ic_wbuf_strb_{0} = 0; _ax4ic_wbuf_last_{0} = 0;\n"
            "    _ax4ic_rbuf_valid_{0} = 0; _ax4ic_rbuf_resp_{0} = 0; _ax4ic_rbuf_last_{0} = 0;\n"
            "    _ax4ic_rbuf_rid_{0} = 0;\n"
            "    _ax4ic_bvalid_{0} = 0; _ax4ic_bdone_{0} = 0; _ax4ic_bresp_{0} = 0;\n"
            "    _ax4ic_bid_{0} = 0; _ax4ic_rdone_{0} = 0; _ax4ic_dec_left_{0} = 0;\n"
            "    _ax4ic_b_got_{0} = 0; _ax4ic_r_got_{0} = 0;\n"
            "    _ax4ic_b_sent_{0} = 0; _ax4ic_r_sent_{0} = 0;\n",
            id);
}

std::string Axi4Interconnect::genFuncDef_seq() const {
    const int id = _id;
    const int M = _num_masters;
    const int S = (int) _segments.size();
    const uint32_t id_mask = (1u << _id_width) - 1u;
    auto in = [&](int idx) { return std::to_string(inputs()[idx]->netId()); };
    auto wire = [&](int idx) {
        const int n = inputs()[idx]->netId();
        return n < 0 ? std::string("0") : std::format("_w[{}]", n);
    };
    auto read1 = [&](int idx) {
        const int n = inputs()[idx]->netId();
        return n < 0 ? std::string("0") : std::format("(_w[{}][0] & 1)", n);
    };
    auto readN = [&](int idx, int width, const std::string &var) {
        const int n = inputs()[idx]->netId();
        if (n < 0) return std::format("uint32_t {} = 0;\n", var);
        return gen_read_wire(n, width, inputs()[idx]->net() ? inputs()[idx]->net()->bit_width() : 0, var);
    };

    std::string c;
    c += std::format("// Axi4Interconnect {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    const bool _clk_pos = " + genGetWireEdge(0, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";
    c += "    const uint8_t _b_got_prev = _ax4ic_b_got_" + std::to_string(id) + ";\n";
    c += "    const uint8_t _r_got_prev = _ax4ic_r_got_" + std::to_string(id) + ";\n";
    c += "    " + readN(1, 1, "_rst") + "\n";

    // 读入所有主口字段到局部数组（生成期展开）
    c += std::format("    uint8_t _awvalid[{}], _wvalid[{}], _bready[{}], _arvalid[{}], _rready[{}];\n", M, M, M, M, M);
    c += std::format("    uint8_t _awlen[{}], _awsize[{}], _awburst[{}], _arlen[{}], _arsize[{}], _arburst[{}];\n", M,
                     M, M, M, M, M);
    c += std::format("    uint8_t _wstrb[{}], _wlast[{}];\n", M, M);
    c += std::format("    uint16_t _awid[{}], _arid[{}];\n", M, M);
    c += std::format("    uint32_t _awaddr[{}], _wdata[{}], _araddr[{}];\n", M, M, M);
    for (int m = 0; m < M; m++) {
        c += std::format("    _awvalid[{}] = {}; _wvalid[{}] = {}; _bready[{}] = {};\n", m,
                         read1(masterInIdx(m, MI_AWVALID)), m, read1(masterInIdx(m, MI_WVALID)), m,
                         read1(masterInIdx(m, MI_BREADY)));
        c += std::format("    _arvalid[{}] = {}; _rready[{}] = {};\n", m,
                         read1(masterInIdx(m, MI_ARVALID)), m, read1(masterInIdx(m, MI_RREADY)));
        c += std::format("    _wlast[{}] = {};\n", m, read1(masterInIdx(m, MI_WLAST)));
        c += std::format("    _awid[{}] = (uint16_t)({}); _arid[{}] = (uint16_t)({});\n", m,
                         read1(masterInIdx(m, MI_AWID)), m, read1(masterInIdx(m, MI_ARID)));
    }
    // 多位字段单独读，避免 read1 只取最低位
    for (int m = 0; m < M; m++) {
        c += "    " + readN(masterInIdx(m, MI_AWLEN), 8, std::format("_awlen_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_AWSIZE), 3, std::format("_awsize_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_AWBURST), 2, std::format("_awburst_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_AWID), _id_width, std::format("_awid_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_AWADDR), 32, std::format("_awaddr_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_WSTRB), 4, std::format("_wstrb_{}", m)) + "\n";
        c += std::format("    _awlen[{0}] = (uint8_t) _awlen_{0}; _awsize[{0}] = (uint8_t) _awsize_{0};\n"
                         "    _awburst[{0}] = (uint8_t) _awburst_{0}; _awid[{0}] = (uint16_t) _awid_{0};\n"
                         "    _awaddr[{0}] = _awaddr_{0}; _wstrb[{0}] = (uint8_t) _wstrb_{0};\n",
                         m);
        c += "    " + readN(masterInIdx(m, MI_WDATA), 32, std::format("_wd_{}", m)) + "\n";
        c += std::format("    _wdata[{}] = _wd_{};\n", m, m);
        c += "    " + readN(masterInIdx(m, MI_ARLEN), 8, std::format("_arlen_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_ARSIZE), 3, std::format("_arsize_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_ARBURST), 2, std::format("_arburst_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_ARID), _id_width, std::format("_arid_{}", m)) + "\n";
        c += "    " + readN(masterInIdx(m, MI_ARADDR), 32, std::format("_araddr_{}", m)) + "\n";
        c += std::format("    _arlen[{0}] = (uint8_t) _arlen_{0}; _arsize[{0}] = (uint8_t) _arsize_{0};\n"
                         "    _arburst[{0}] = (uint8_t) _arburst_{0}; _arid[{0}] = (uint16_t) _arid_{0};\n"
                         "    _araddr[{0}] = _araddr_{0};\n",
                         m);
    }

    // 读入从口输入
    c += std::format("    uint8_t _s_awready[{}], _s_wready[{}], _s_bvalid[{}], _s_bresp[{}];\n", S, S, S, S);
    c += std::format("    uint8_t _s_arready[{}], _s_rvalid[{}], _s_rresp[{}], _s_rlast[{}];\n", S, S, S, S);
    c += std::format("    uint16_t _s_bid[{}], _s_rid[{}];\n", S, S);
    c += std::format("    uint32_t _s_rdata[{}];\n", S);
    for (int s = 0; s < S; s++) {
        c += std::format("    _s_awready[{0}] = {1}; _s_wready[{0}] = {2}; _s_bvalid[{0}] = {3};\n",
                         s, read1(slaveInIdx(s, SI_AWREADY)), read1(slaveInIdx(s, SI_WREADY)),
                         read1(slaveInIdx(s, SI_BVALID)));
        c += std::format("    _s_arready[{0}] = {1}; _s_rvalid[{0}] = {2}; _s_rlast[{0}] = {3};\n",
                         s, read1(slaveInIdx(s, SI_ARREADY)), read1(slaveInIdx(s, SI_RVALID)),
                         read1(slaveInIdx(s, SI_RLAST)));
        c += "    " + readN(slaveInIdx(s, SI_BRESP), 2, std::format("_bresp_{}", s)) + "\n";
        c += "    " + readN(slaveInIdx(s, SI_RRESP), 2, std::format("_rresp_{}", s)) + "\n";
        c += std::format("    _s_bresp[{0}] = (uint8_t) _bresp_{0}; _s_rresp[{0}] = (uint8_t) _rresp_{0};\n", s);
        c += "    " + readN(slaveInIdx(s, SI_BID), _slave_id_width, std::format("_bid_{}", s)) + "\n";
        c += "    " + readN(slaveInIdx(s, SI_RID), _slave_id_width, std::format("_rid_{}", s)) + "\n";
        c += "    " + readN(slaveInIdx(s, SI_RDATA), 32, std::format("_rd_{}", s)) + "\n";
        c += std::format("    _s_bid[{0}] = (uint16_t) _bid_{0}; _s_rid[{0}] = (uint16_t) _rid_{0}; "
                         "_s_rdata[{0}] = _rd_{0};\n",
                         s);
    }

    // 输出默认值
    c += std::format("    uint8_t _o_awready[{}], _o_wready[{}], _o_bvalid[{}], _o_bresp[{}];\n", M, M, M, M);
    c += std::format("    uint8_t _o_arready[{}], _o_rvalid[{}], _o_rresp[{}], _o_rlast[{}];\n", M, M, M, M);
    c += std::format("    uint16_t _o_bid[{}], _o_rid[{}];\n", M, M);
    c += std::format("    uint32_t _o_rdata[{}];\n", M);
    c += std::format("    for (int _i = 0; _i < {}; _i++) {{ _o_awready[_i]=0; _o_wready[_i]=0; _o_bvalid[_i]=0;"
                     " _o_bresp[_i]=0; _o_arready[_i]=0; _o_rvalid[_i]=0; _o_rresp[_i]=0; _o_rlast[_i]=0;"
                     " _o_bid[_i]=0; _o_rid[_i]=0; _o_rdata[_i]=0; }}\n",
                     M);

    // 复位
    c += std::format(
            "    if (_rst) {{\n"
            "        _ax4ic_wst_{0}=0; _ax4ic_rst2_{0}=0; _ax4ic_ws_{0}=-1; _ax4ic_rs_{0}=-1;\n"
            "        _ax4ic_wbuf_valid_{0}=0; _ax4ic_rbuf_valid_{0}=0; _ax4ic_bvalid_{0}=0;\n"
            "        _ax4ic_bdone_{0}=0; _ax4ic_rdone_{0}=0; _ax4ic_dec_left_{0}=0;\n"
            "        _ax4ic_b_got_{0}=0; _ax4ic_r_got_{0}=0;\n"
            "        _ax4ic_b_sent_{0}=0; _ax4ic_r_sent_{0}=0;\n"
            "        goto _ax4ic_out_{0};\n"
            "    }}\n",
            id);

    // ---- 写路径：空闲扫描主口 ----
    c += "    int _wsel = -1;\n";
    c += "    if (_ax4ic_wst_" + std::to_string(id) + " == 0) {\n";
    for (int m = 0; m < M; m++)
        c += std::format("        if (_wsel < 0 && _awvalid[{}]) _wsel = {};\n", m, m);
    c += "    }\n";
    c += "    if (_ax4ic_wst_" + std::to_string(id) + " == 0 && _wsel >= 0) {\n";
    c += std::format("        _ax4ic_wm_{0} = _wsel;\n", id);
    c += std::format("        _ax4ic_wid_{0} = _awid[_wsel];\n", id);
    c += std::format("        _ax4ic_waddr_{0} = _awaddr[_wsel];\n", id);
    c += std::format("        _ax4ic_wlen_{0} = _awlen[_wsel];\n", id);
    c += std::format("        _ax4ic_wsize_{0} = _awsize[_wsel];\n", id);
    c += std::format("        _ax4ic_wburst_{0} = _awburst[_wsel];\n", id);
    c += genDecode(std::format("_ax4ic_waddr_{}", id), id);
    c += std::format("        _ax4ic_ws_{0} = _seg{0};\n", id);
    c += "        _o_awready[_ax4ic_wm_" + std::to_string(id) + "] = 1;\n";
    c += std::format("        _ax4ic_wbuf_valid_{0} = 0;\n", id);
    c += std::format("        _ax4ic_wst_{0} = (_ax4ic_ws_{0} >= 0) ? 1 : 4;\n", id);
    c += "    }\n";

    // AW 转发
    c += std::format(
            "    if (_ax4ic_wst_{0} == 1) {{\n"
            "        if (_ax4ic_ws_{0} >= 0 && _s_awready[_ax4ic_ws_{0}]) _ax4ic_wst_{0} = 2;\n"
            "    }}\n",
            id);

    // W 转发：先把上一拍锁进缓冲的 beat 送到从口，再接收主口的下一 beat。
    // 顺序不能反：同一拍刚锁存的 beat 还没在从口侧出现过，立刻按从口的 WREADY
    // 判定握手会把它当成已发送，最后一个 beat 因此永远到不了从设备。
    c += std::format(
            "    if (_ax4ic_wst_{0} == 2) {{\n"
            "        if (_ax4ic_wbuf_valid_{0} && _ax4ic_ws_{0} >= 0 && _s_wready[_ax4ic_ws_{0}]) {{\n"
            "            if (_ax4ic_wbuf_last_{0}) {{ _ax4ic_wst_{0} = 3; }}\n"
            "            else {{ _ax4ic_wbuf_valid_{0} = 0; }}\n"
            "        }}\n"
            "        if (_ax4ic_wst_{0} == 2 && !_ax4ic_wbuf_valid_{0} && _wvalid[_ax4ic_wm_{0}]) {{\n"
            "            uint32_t _wd = _wdata[_ax4ic_wm_{0}];\n"
            "            dcs_memcpy(_ax4ic_wbuf_{0}, &_wd, 4);\n"
            "            _ax4ic_wbuf_strb_{0} = _wstrb[_ax4ic_wm_{0}];\n"
            "            _ax4ic_wbuf_last_{0} = _wlast[_ax4ic_wm_{0}];\n"
            "            _ax4ic_wbuf_valid_{0} = 1;\n"
            "            _o_wready[_ax4ic_wm_{0}] = 1;\n"
            "        }}\n"
            "    }}\n",
            id);

    // B 转发：本拍从从口接收响应，下一拍才送给主口；同拍送出会被主口撤销的
    // READY 判成"已被接收"，主口其实没看到 VALID
    c += std::format(
            "    if (_ax4ic_wst_{0} == 3) {{\n"
            "        if (!_ax4ic_b_got_{0} && _ax4ic_ws_{0} >= 0 && _s_bvalid[_ax4ic_ws_{0}]) {{\n"
            "            _ax4ic_bresp_{0} = _s_bresp[_ax4ic_ws_{0}];\n"
            "            _ax4ic_bid_{0} = (uint16_t)(_s_bid[_ax4ic_ws_{0}] & 0x{1:X}u);\n"
            "            _ax4ic_b_got_{0} = 1; _ax4ic_b_sent_{0} = 0;\n"
            "        }}\n"
            "        if (_b_got_prev) {{\n"
            "            _o_bvalid[_ax4ic_wm_{0}] = 1;\n"
            "            _o_bid[_ax4ic_wm_{0}] = _ax4ic_bid_{0};\n"
            "            _o_bresp[_ax4ic_wm_{0}] = _ax4ic_bresp_{0};\n"
            "            if (!_ax4ic_b_sent_{0}) {{\n"
            "                _ax4ic_bdone_{0} = _bready[_ax4ic_wm_{0}];\n"
            "                _ax4ic_b_sent_{0} = 1;\n"
            "            }} else if (_ax4ic_bdone_{0} || _bready[_ax4ic_wm_{0}]) {{\n"
            "                _o_bvalid[_ax4ic_wm_{0}] = 0;\n"
            "                _ax4ic_b_got_{0} = 0; _ax4ic_b_sent_{0} = 0; _ax4ic_bdone_{0} = 0;\n"
            "                _ax4ic_wst_{0} = 0; _ax4ic_wbuf_valid_{0} = 0;\n"
            "            }}\n"
            "        }}\n"
            "    }}\n",
            id, id_mask);

    // DECERR 写：收完所有 W beat 后回 DECERR
    c += std::format(
            "    if (_ax4ic_wst_{0} == 4) {{\n"
            "        if (_wvalid[_ax4ic_wm_{0}]) {{\n"
            "            _o_wready[_ax4ic_wm_{0}] = 1;\n"
            "            if (_wlast[_ax4ic_wm_{0}]) {{\n"
            "                _ax4ic_bresp_{0} = 3; _ax4ic_bid_{0} = _ax4ic_wid_{0};\n"
            "                _ax4ic_b_got_{0} = 1; _ax4ic_b_sent_{0} = 0;\n"
            "                _ax4ic_wst_{0} = 5;\n"
            "            }}\n"
            "        }}\n"
            "    }}\n"
            "    if (_ax4ic_wst_{0} == 5) {{\n"
            "        if (_b_got_prev) {{\n"
            "            _o_bvalid[_ax4ic_wm_{0}] = 1;\n"
            "            _o_bid[_ax4ic_wm_{0}] = _ax4ic_bid_{0};\n"
            "            _o_bresp[_ax4ic_wm_{0}] = _ax4ic_bresp_{0};\n"
            "            if (!_ax4ic_b_sent_{0}) {{\n"
            "                _ax4ic_bdone_{0} = _bready[_ax4ic_wm_{0}];\n"
            "                _ax4ic_b_sent_{0} = 1;\n"
            "            }} else if (_ax4ic_bdone_{0} || _bready[_ax4ic_wm_{0}]) {{\n"
            "                _o_bvalid[_ax4ic_wm_{0}] = 0;\n"
            "                _ax4ic_b_got_{0} = 0; _ax4ic_b_sent_{0} = 0; _ax4ic_bdone_{0} = 0;\n"
            "                _ax4ic_wst_{0} = 0;\n"
            "            }}\n"
            "        }}\n"
            "    }}\n",
            id);

    // ---- 读路径 ----
    c += "    int _rsel = -1;\n";
    c += "    if (_ax4ic_rst2_" + std::to_string(id) + " == 0) {\n";
    for (int m = 0; m < M; m++)
        c += std::format("        if (_rsel < 0 && _arvalid[{}]) _rsel = {};\n", m, m);
    c += "    }\n";
    c += "    if (_ax4ic_rst2_" + std::to_string(id) + " == 0 && _rsel >= 0) {\n";
    c += std::format("        _ax4ic_rm_{0} = _rsel;\n", id);
    c += std::format("        _ax4ic_rid_{0} = _arid[_rsel];\n", id);
    c += std::format("        _ax4ic_raddr_{0} = _araddr[_rsel];\n", id);
    c += std::format("        _ax4ic_rlen_{0} = _arlen[_rsel];\n", id);
    c += std::format("        _ax4ic_rsize_{0} = _arsize[_rsel];\n", id);
    c += std::format("        _ax4ic_rburst_{0} = _arburst[_rsel];\n", id);
    c += genDecode(std::format("_ax4ic_raddr_{}", id), id);
    c += std::format("        _ax4ic_rs_{0} = _seg{0};\n", id);
    c += "        _o_arready[_ax4ic_rm_" + std::to_string(id) + "] = 1;\n";
    c += std::format("        _ax4ic_rbuf_valid_{0} = 0;\n", id);
    c += std::format("        if (_ax4ic_rs_{0} >= 0) _ax4ic_rst2_{0} = 1;\n", id);
    c += std::format(
            "        else {{ _ax4ic_rst2_{0} = 3; _ax4ic_dec_left_{0} = (uint32_t)_arlen[_rsel] + 1u; }}\n", id);
    c += "    }\n";

    // AR 转发
    c += std::format(
            "    if (_ax4ic_rst2_{0} == 1) {{\n"
            "        if (_ax4ic_rs_{0} >= 0 && _s_arready[_ax4ic_rs_{0}]) _ax4ic_rst2_{0} = 2;\n"
            "    }}\n",
            id);

    // R 转发：同样是"本拍收、下一拍送"
    c += std::format(
            "    if (_ax4ic_rst2_{0} == 2) {{\n"
            "        if (!_ax4ic_r_got_{0} && _ax4ic_rs_{0} >= 0 && _s_rvalid[_ax4ic_rs_{0}]) {{\n"
            "            uint32_t _rd = _s_rdata[_ax4ic_rs_{0}];\n"
            "            dcs_memcpy(_ax4ic_rbuf_{0}, &_rd, 4);\n"
            "            _ax4ic_rbuf_resp_{0} = _s_rresp[_ax4ic_rs_{0}];\n"
            "            _ax4ic_rbuf_last_{0} = _s_rlast[_ax4ic_rs_{0}];\n"
            "            _ax4ic_rbuf_rid_{0} = (uint16_t)(_s_rid[_ax4ic_rs_{0}] & 0x{1:X}u);\n"
            "            _ax4ic_r_got_{0} = 1; _ax4ic_r_sent_{0} = 0;\n"
            "        }}\n"
            "        if (_r_got_prev) {{\n"
            "            _o_rvalid[_ax4ic_rm_{0}] = 1;\n"
            "            _o_rid[_ax4ic_rm_{0}] = _ax4ic_rbuf_rid_{0};\n"
            "            dcs_memcpy(&_o_rdata[_ax4ic_rm_{0}], _ax4ic_rbuf_{0}, 4);\n"
            "            _o_rresp[_ax4ic_rm_{0}] = _ax4ic_rbuf_resp_{0};\n"
            "            _o_rlast[_ax4ic_rm_{0}] = _ax4ic_rbuf_last_{0};\n"
            "            if (!_ax4ic_r_sent_{0}) {{\n"
            "                _ax4ic_rdone_{0} = _rready[_ax4ic_rm_{0}];\n"
            "                _ax4ic_r_sent_{0} = 1;\n"
            "            }} else if (_ax4ic_rdone_{0} || _rready[_ax4ic_rm_{0}]) {{\n"
            "                _o_rvalid[_ax4ic_rm_{0}] = 0;\n"
            "                _ax4ic_r_got_{0} = 0; _ax4ic_r_sent_{0} = 0; _ax4ic_rdone_{0} = 0;\n"
            "                if (_ax4ic_rbuf_last_{0}) {{ _ax4ic_rst2_{0} = 0; _ax4ic_rbuf_last_{0} = 0; }}\n"
            "            }}\n"
            "        }}\n"
            "    }}\n",
            id, id_mask);

    // DECERR 读：按 ARLEN+1 个 beat 返回，同样"本拍准备、下一拍送出"
    c += std::format(
            "    if (_ax4ic_rst2_{0} == 3) {{\n"
            "        if (!_ax4ic_r_got_{0} && _ax4ic_dec_left_{0} > 0) {{\n"
            "            _ax4ic_rbuf_last_{0} = (_ax4ic_dec_left_{0} <= 1u) ? 1u : 0u;\n"
            "            _ax4ic_rbuf_rid_{0} = _ax4ic_rid_{0};\n"
            "            _ax4ic_r_got_{0} = 1; _ax4ic_r_sent_{0} = 0;\n"
            "        }}\n"
            "        if (_r_got_prev) {{\n"
            "            _o_rvalid[_ax4ic_rm_{0}] = 1;\n"
            "            _o_rid[_ax4ic_rm_{0}] = _ax4ic_rbuf_rid_{0};\n"
            "            _o_rdata[_ax4ic_rm_{0}] = 0;\n"
            "            _o_rresp[_ax4ic_rm_{0}] = 3;\n"
            "            _o_rlast[_ax4ic_rm_{0}] = _ax4ic_rbuf_last_{0};\n"
            "            if (!_ax4ic_r_sent_{0}) {{\n"
            "                _ax4ic_rdone_{0} = _rready[_ax4ic_rm_{0}];\n"
            "                _ax4ic_r_sent_{0} = 1;\n"
            "            }} else if (_ax4ic_rdone_{0} || _rready[_ax4ic_rm_{0}]) {{\n"
            "                _o_rvalid[_ax4ic_rm_{0}] = 0;\n"
            "                _ax4ic_r_got_{0} = 0; _ax4ic_r_sent_{0} = 0; _ax4ic_rdone_{0} = 0;\n"
            "                _ax4ic_dec_left_{0} = _ax4ic_dec_left_{0} - 1u;\n"
            "                if (_ax4ic_dec_left_{0} == 0u) {{ _ax4ic_rst2_{0} = 0; _ax4ic_rbuf_last_{0} = 0; }}\n"
            "            }}\n"
            "        }}\n"
            "    }}\n",
            id);

    // ---- 从口输出 ----
    c += std::format("_ax4ic_out_{}:\n    ;\n", id);
    c += std::format(
            "    uint8_t _s_o_awvalid[{}], _s_o_wvalid[{}], _s_o_bready[{}], _s_o_arvalid[{}], _s_o_rready[{}];\n",
            S, S, S, S, S);
    c += std::format("    for (int _i = 0; _i < {}; _i++) {{ _s_o_awvalid[_i]=0; _s_o_wvalid[_i]=0;"
                     " _s_o_bready[_i]=0; _s_o_arvalid[_i]=0; _s_o_rready[_i]=0; }}\n",
                     S);
    c += std::format(
            "    if (_ax4ic_ws_{0} >= 0) {{\n"
            "        if (_ax4ic_wst_{0} == 1) _s_o_awvalid[_ax4ic_ws_{0}] = 1;\n"
            "        if (_ax4ic_wst_{0} == 2) _s_o_wvalid[_ax4ic_ws_{0}] = _ax4ic_wbuf_valid_{0};\n"
            "        if (_ax4ic_wst_{0} == 3) _s_o_bready[_ax4ic_ws_{0}] = !_b_got_prev;\n"
            "    }}\n"
            "    if (_ax4ic_rs_{0} >= 0) {{\n"
            "        if (_ax4ic_rst2_{0} == 1) _s_o_arvalid[_ax4ic_rs_{0}] = 1;\n"
            "        if (_ax4ic_rst2_{0} == 2) _s_o_rready[_ax4ic_rs_{0}] = !_r_got_prev;\n"
            "    }}\n",
            id);

    // 主口输出
    for (int m = 0; m < M; m++) {
        c += "    " + genOutputWrite(masterOutIdx(m, MO_AWREADY), "_o_awready[" + std::to_string(m) + "]", 1) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_WREADY), "_o_wready[" + std::to_string(m) + "]", 1) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_BID), "_o_bid[" + std::to_string(m) + "]", _id_width) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_BRESP), "_o_bresp[" + std::to_string(m) + "]", 2) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_BVALID), "_o_bvalid[" + std::to_string(m) + "]", 1) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_ARREADY), "_o_arready[" + std::to_string(m) + "]", 1) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_RID), "_o_rid[" + std::to_string(m) + "]", _id_width) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_RDATA), "_o_rdata[" + std::to_string(m) + "]", 32) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_RRESP), "_o_rresp[" + std::to_string(m) + "]", 2) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_RLAST), "_o_rlast[" + std::to_string(m) + "]", 1) + "\n";
        c += "    " + genOutputWrite(masterOutIdx(m, MO_RVALID), "_o_rvalid[" + std::to_string(m) + "]", 1) + "\n";
    }
    // 从口输出
    for (int s = 0; s < S; s++) {
        const std::string sid = std::to_string(id);
        c += std::format("    {{ uint16_t _sid = (uint16_t)(((uint32_t)_ax4ic_wm_{0} << {1}) | _ax4ic_wid_{0});\n",
                         sid, _id_width);
        c += "      " + genOutputWrite(slaveOutIdx(s, SO_AWID), "_sid", _slave_id_width) + " }\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_AWADDR), "_ax4ic_waddr_" + sid, 32) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_AWLEN), "_ax4ic_wlen_" + sid, 8) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_AWSIZE), "_ax4ic_wsize_" + sid, 3) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_AWBURST), "_ax4ic_wburst_" + sid, 2) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_AWVALID), "_s_o_awvalid[" + std::to_string(s) + "]", 1) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_WDATA), "_ax4ic_wbuf_" + sid, 32) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_WSTRB), "_ax4ic_wbuf_strb_" + sid, 4) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_WLAST), "_ax4ic_wbuf_last_" + sid, 1) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_WVALID), "_s_o_wvalid[" + std::to_string(s) + "]", 1) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_BREADY), "_s_o_bready[" + std::to_string(s) + "]", 1) + "\n";
        c += std::format("    {{ uint16_t _sid = (uint16_t)(((uint32_t)_ax4ic_rm_{0} << {1}) | _ax4ic_rid_{0});\n",
                         sid, _id_width);
        c += "      " + genOutputWrite(slaveOutIdx(s, SO_ARID), "_sid", _slave_id_width) + " }\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_ARADDR), "_ax4ic_raddr_" + sid, 32) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_ARLEN), "_ax4ic_rlen_" + sid, 8) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_ARSIZE), "_ax4ic_rsize_" + sid, 3) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_ARBURST), "_ax4ic_rburst_" + sid, 2) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_ARVALID), "_s_o_arvalid[" + std::to_string(s) + "]", 1) + "\n";
        c += "    " + genOutputWrite(slaveOutIdx(s, SO_RREADY), "_s_o_rready[" + std::to_string(s) + "]", 1) + "\n";
    }
    c += "}\n";
    (void) in;
    return c;
}

std::unique_ptr<Component> Axi4Interconnect::clone(const std::string &new_name) const {
    std::string segs;
    for (size_t i = 0; i < _segments.size(); i++) {
        if (i) segs += ",";
        segs += std::to_string(_segments[i].base) + ":" + std::to_string(_segments[i].size);
    }
    return std::make_unique<Axi4Interconnect>(new_name, segs, _num_masters, _id_width);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
        {"axi4_ic",
         {{"segments", "0:4294967296"}, {"num_masters", "1"}, {"id_width", "4"}},
         {}, {}, dsc::ComponentCategory::Bus, "AXI4 多主多从互连，按地址段路由并扩展事务 ID"},
        [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            auto get = [&](const char *k, const char *d) {
                auto it = p.find(k);
                return it == p.end() ? std::string(d) : it->second;
            };
            return std::make_unique<dsc::Axi4Interconnect>(n, get("segments", "0:4294967296"),
                                                           std::stoi(get("num_masters", "1")),
                                                           std::stoi(get("id_width", "4")));
        });
} // namespace
