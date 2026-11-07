// Axi4MasterAdapter 实现：把单次 mem_* 事务翻译成 AXI4 单拍事务。
#include "dcs/components/platform/Axi4MasterAdapter.h"

#include <format>

#include "dcs/ComponentFactory.h"

namespace dsc {

namespace {
enum InIdx {
    I_CLK = 0, I_RST,
    I_MEM_ADR, I_MEM_WDAT, I_MEM_WE, I_MEM_SEL, I_MEM_REQ, I_MEM_LOCK,
    I_AWREADY, I_WREADY, I_BID, I_BRESP, I_BVALID,
    I_RDATA, I_RRESP, I_RID, I_RLAST, I_RVALID, I_ARREADY
};
enum OutIdx {
    O_AWID = 0, O_AWADDR, O_AWLEN, O_AWSIZE, O_AWBURST, O_AWVALID,
    O_WDATA, O_WSTRB, O_WLAST, O_WVALID, O_BREADY,
    O_ARID, O_ARADDR, O_ARLEN, O_ARSIZE, O_ARBURST, O_ARVALID, O_RREADY,
    O_MEM_ACK, O_MEM_RDAT
};

// 0 空闲，1 写等响应，2 读等响应，3 等 CPU 撤销 mem_req
enum St { ST_IDLE = 0, ST_WRITE = 1, ST_READ = 2, ST_RESP = 3 };
} // namespace

Axi4MasterAdapter::Axi4MasterAdapter(const std::string &name, int id_width)
    : SequentialComponent(name, "axi4_master_adapter"), _id_width(id_width) {
    setParam("id_width", std::to_string(id_width));

    addInput("clk", 1, PinEdge::Rising);
    addInput("rst", 1);
    addInput("mem_adr", 32);
    addInput("mem_wdat", 32);
    addInput("mem_we", 1);
    addInput("mem_sel", 4);
    addInput("mem_req", 1);
    addInput("mem_lock", 1);
    addInput("m_axi_awready", 1);
    addInput("m_axi_wready", 1);
    addInput("m_axi_bid", id_width);
    addInput("m_axi_bresp", 2);
    addInput("m_axi_bvalid", 1);
    addInput("m_axi_rdata", 32);
    addInput("m_axi_rresp", 2);
    addInput("m_axi_rid", id_width);
    addInput("m_axi_rlast", 1);
    addInput("m_axi_rvalid", 1);
    addInput("m_axi_arready", 1);

    addOutput("m_axi_awid", id_width, false, true);
    addOutput("m_axi_awaddr", 32, false, true);
    addOutput("m_axi_awlen", 8, false, true);
    addOutput("m_axi_awsize", 3, false, true);
    addOutput("m_axi_awburst", 2, false, true);
    addOutput("m_axi_awvalid", 1, false, true);
    addOutput("m_axi_wdata", 32, false, true);
    addOutput("m_axi_wstrb", 4, false, true);
    addOutput("m_axi_wlast", 1, false, true);
    addOutput("m_axi_wvalid", 1, false, true);
    addOutput("m_axi_bready", 1, false, true);
    addOutput("m_axi_arid", id_width, false, true);
    addOutput("m_axi_araddr", 32, false, true);
    addOutput("m_axi_arlen", 8, false, true);
    addOutput("m_axi_arsize", 3, false, true);
    addOutput("m_axi_arburst", 2, false, true);
    addOutput("m_axi_arvalid", 1, false, true);
    addOutput("m_axi_rready", 1, false, true);
    addOutput("mem_ack", 1, false, true);
    addOutput("mem_rdat", 32, false, true);
}

Axi4MasterAdapter::~Axi4MasterAdapter() = default;

std::string Axi4MasterAdapter::genStateDecl() const {
    return std::format(
            "static uint32_t _ax4m_addr_{0}, _ax4m_wdat_{0}, _ax4m_rdat_{0};\n"
            "static uint8_t _ax4m_sel_{0}, _ax4m_shift_{0};\n"
            "static uint8_t _ax4m_awvalid_{0}, _ax4m_wvalid_{0}, _ax4m_bready_{0};\n"
            "static uint8_t _ax4m_arvalid_{0}, _ax4m_rready_{0};\n"
            "static uint8_t _ax4m_ack_{0}, _ax4m_st_{0};\n",
            _id);
}

std::string Axi4MasterAdapter::genInitCode() const {
    return std::format(
            "    _ax4m_addr_{0} = 0; _ax4m_wdat_{0} = 0; _ax4m_rdat_{0} = 0;\n"
            "    _ax4m_sel_{0} = 0; _ax4m_shift_{0} = 0;\n"
            "    _ax4m_awvalid_{0} = 0; _ax4m_wvalid_{0} = 0; _ax4m_bready_{0} = 0;\n"
            "    _ax4m_arvalid_{0} = 0; _ax4m_rready_{0} = 0;\n"
            "    _ax4m_ack_{0} = 0; _ax4m_st_{0} = 0;\n",
            _id);
}

std::string Axi4MasterAdapter::genFuncDef_seq() const {
    const int id = _id;
    auto read = [&](int pin, int width, const char *var) {
        const int net_w = inputs()[pin]->net() ? inputs()[pin]->net()->bit_width() : 0;
        return gen_read_wire(inputs()[pin]->netId(), width, net_w, var);
    };

    std::string c;
    c += std::format("// Axi4MasterAdapter {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    const bool _clk_pos = " + genGetWireEdge(I_CLK, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";
    // genOutputWrite 要求能取地址的名称，常量字段走这几个局部变量
    c += "    uint8_t _zero = 0, _one = 1, _two = 2;\n";

    c += "    " + read(I_RST, 1, "_rst") + "\n";
    c += "    " + read(I_MEM_ADR, 32, "_adr") + "\n";
    c += "    " + read(I_MEM_WDAT, 32, "_wdat") + "\n";
    c += "    " + read(I_MEM_WE, 1, "_we") + "\n";
    c += "    " + read(I_MEM_SEL, 4, "_sel") + "\n";
    c += "    " + read(I_MEM_REQ, 1, "_req") + "\n";
    c += "    " + read(I_AWREADY, 1, "_awready") + "\n";
    c += "    " + read(I_WREADY, 1, "_wready") + "\n";
    c += "    " + read(I_BVALID, 1, "_bvalid") + "\n";
    c += "    " + read(I_ARREADY, 1, "_arready") + "\n";
    c += "    " + read(I_RDATA, 32, "_rdata") + "\n";
    c += "    " + read(I_RVALID, 1, "_rvalid") + "\n";

    c += std::format(
            "    if (_rst) {{\n"
            "        _ax4m_awvalid_{0} = 0; _ax4m_wvalid_{0} = 0; _ax4m_bready_{0} = 0;\n"
            "        _ax4m_arvalid_{0} = 0; _ax4m_rready_{0} = 0;\n"
            "        _ax4m_ack_{0} = 0; _ax4m_st_{0} = 0;\n"
            "        goto _ax4m_out_{0};\n"
            "    }}\n",
            id);

    // 发起事务：地址未对齐时，AXI 的数据要放到地址对应的字节通道上，WSTRB 同步左移。
    // CPU 的 mem_sel 是 Wishbone 语义（第 i 位对应 adr+i），移位后正好落到 AXI lane。
    c += std::format(
            "    if (_ax4m_st_{0} == 0 && _req) {{\n"
            "        _ax4m_addr_{0} = _adr;\n"
            "        _ax4m_shift_{0} = (uint8_t)(_adr & 3u);\n"
            "        if (_we) {{\n"
            "            _ax4m_wdat_{0} = _wdat << (8u * _ax4m_shift_{0});\n"
            "            _ax4m_sel_{0} = (uint8_t)((_sel << _ax4m_shift_{0}) & 0x0Fu);\n"
            "            _ax4m_awvalid_{0} = 1; _ax4m_wvalid_{0} = 1; _ax4m_bready_{0} = 1;\n"
            "            _ax4m_st_{0} = 1;\n"
            "        }} else {{\n"
            "            _ax4m_arvalid_{0} = 1; _ax4m_rready_{0} = 1;\n"
            "            _ax4m_st_{0} = 2;\n"
            "        }}\n"
            "    }}\n"
            "    if (_ax4m_st_{0} == 1) {{\n"
            "        if (_ax4m_awvalid_{0} && _awready) _ax4m_awvalid_{0} = 0;\n"
            "        if (_ax4m_wvalid_{0} && _wready) _ax4m_wvalid_{0} = 0;\n"
            "        if (!_ax4m_awvalid_{0} && !_ax4m_wvalid_{0} && _bvalid) {{\n"
            "            _ax4m_bready_{0} = 0; _ax4m_ack_{0} = 1; _ax4m_st_{0} = 3;\n"
            "        }}\n"
            "    }} else if (_ax4m_st_{0} == 2) {{\n"
            "        if (_ax4m_arvalid_{0} && _arready) _ax4m_arvalid_{0} = 0;\n"
            "        if (!_ax4m_arvalid_{0} && _rvalid) {{\n"
            "            _ax4m_rdat_{0} = _rdata >> (8u * _ax4m_shift_{0});\n"
            "            _ax4m_rready_{0} = 0; _ax4m_ack_{0} = 1; _ax4m_st_{0} = 3;\n"
            "        }}\n"
            "    }} else if (_ax4m_st_{0} == 3 && !_req) {{\n"
            "        _ax4m_ack_{0} = 0; _ax4m_st_{0} = 0;\n"
            "    }}\n",
            id);

    c += std::format("_ax4m_out_{}:\n    ;\n", id);
    c += "    " + genOutputWrite(O_AWID, "_zero", _id_width) + "\n";
    c += "    " + genOutputWrite(O_AWADDR, "_ax4m_addr_" + std::to_string(id), 32) + "\n";
    c += "    " + genOutputWrite(O_AWLEN, "_zero", 8) + "\n";
    c += "    " + genOutputWrite(O_AWSIZE, "_two", 3) + "\n";
    c += "    " + genOutputWrite(O_AWBURST, "_one", 2) + "\n";
    c += "    " + genOutputWrite(O_AWVALID, "_ax4m_awvalid_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_WDATA, "_ax4m_wdat_" + std::to_string(id), 32) + "\n";
    c += "    " + genOutputWrite(O_WSTRB, "_ax4m_sel_" + std::to_string(id), 4) + "\n";
    c += "    " + genOutputWrite(O_WLAST, "_one", 1) + "\n";
    c += "    " + genOutputWrite(O_WVALID, "_ax4m_wvalid_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_BREADY, "_ax4m_bready_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_ARID, "_zero", _id_width) + "\n";
    c += "    " + genOutputWrite(O_ARADDR, "_ax4m_addr_" + std::to_string(id), 32) + "\n";
    c += "    " + genOutputWrite(O_ARLEN, "_zero", 8) + "\n";
    c += "    " + genOutputWrite(O_ARSIZE, "_two", 3) + "\n";
    c += "    " + genOutputWrite(O_ARBURST, "_one", 2) + "\n";
    c += "    " + genOutputWrite(O_ARVALID, "_ax4m_arvalid_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_RREADY, "_ax4m_rready_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_MEM_ACK, "_ax4m_ack_" + std::to_string(id), 1) + "\n";
    c += "    " + genOutputWrite(O_MEM_RDAT, "_ax4m_rdat_" + std::to_string(id), 32) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> Axi4MasterAdapter::clone(const std::string &new_name) const {
    return std::make_unique<Axi4MasterAdapter>(new_name, _id_width);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
        {"axi4_master_adapter",
         {{"id_width", "4"}},
         {"clk", "rst", "mem_adr", "mem_wdat", "mem_we", "mem_sel", "mem_req", "mem_lock", "m_axi_awready",
          "m_axi_wready", "m_axi_bid", "m_axi_bresp", "m_axi_bvalid", "m_axi_rdata", "m_axi_rresp", "m_axi_rid",
          "m_axi_rlast", "m_axi_rvalid", "m_axi_arready"},
         {"m_axi_awid", "m_axi_awaddr", "m_axi_awlen", "m_axi_awsize", "m_axi_awburst", "m_axi_awvalid",
          "m_axi_wdata", "m_axi_wstrb", "m_axi_wlast", "m_axi_wvalid", "m_axi_bready", "m_axi_arid",
          "m_axi_araddr", "m_axi_arlen", "m_axi_arsize", "m_axi_arburst", "m_axi_arvalid", "m_axi_rready",
          "mem_ack", "mem_rdat"},
         dsc::ComponentCategory::Bus, "总线中立的存储器事务接口到 AXI4 主设备（单拍）"},
        [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            auto it = p.find("id_width");
            const int idw = it == p.end() ? 4 : std::stoi(it->second);
            return std::make_unique<dsc::Axi4MasterAdapter>(n, idw);
        });
} // namespace
