#include "dcs/ComponentFactory.h"
#include "dcs/components/platform/WbMasterAdapter.h"

#include <format>

namespace dsc {

namespace {
// 输入按 addInput 顺序
constexpr int I_MEM_ADR = 0, I_MEM_WDAT = 1, I_MEM_WE = 2, I_MEM_SEL = 3, I_MEM_REQ = 4, I_MEM_LOCK = 5;
constexpr int I_WB_ACK = 6, I_WB_DAT = 7;
// 输出按 addOutput 顺序
constexpr int O_WB_ADR = 0, O_WB_DAT = 1, O_WB_WE = 2, O_WB_SEL = 3, O_WB_STB = 4, O_WB_CYC = 5;
constexpr int O_MEM_ACK = 6, O_MEM_RDAT = 7;
} // namespace

WbMasterAdapter::WbMasterAdapter(const std::string &name) : Component(name, "wb_master_adapter") {
    addInput("mem_adr", 32);
    addInput("mem_wdat", 32);
    addInput("mem_we", 1);
    addInput("mem_sel", 4);
    addInput("mem_req", 1);
    addInput("mem_lock", 1);
    addInput("wb_ack_i", 1);
    addInput("wb_dat_i", 32);
    addOutput("wb_adr_o", 32, false, true);
    addOutput("wb_dat_o", 32, false, true);
    addOutput("wb_we_o", 1, false, true);
    addOutput("wb_sel_o", 4, false, true);
    addOutput("wb_stb_o", 1, false, true);
    addOutput("wb_cyc_o", 1, false, true);
    // 应答方向是纯组合透传：标成时序会让它晚一拍才进 _w，读数据就比直连多一拍
    addOutput("mem_ack", 1);
    addOutput("mem_rdat", 32);
}

WbMasterAdapter::~WbMasterAdapter() = default;

namespace {
std::string readPin(const Component &c, int pin, const char *var, int width) {
    const Pin *p = c.inputs()[pin].get();
    const int net_w = p->net() ? p->net()->bit_width() : 0;
    return gen_read_wire(p->netId(), width, net_w, var);
}
} // namespace

std::string WbMasterAdapter::genFuncDef_seq() const {
    std::string c;
    c += std::format("// WbMasterAdapter {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    // 请求方向：主设备本拍已经把 mem_* 直接写进 _w，这里原样转发；
    // mem_lock（事务占用）对应 Wishbone CYC，mem_req（本次传输）对应 STB
    c += "    " + readPin(*this, I_MEM_ADR, "_adr", 32) + "\n";
    c += "    " + readPin(*this, I_MEM_WDAT, "_wdat", 32) + "\n";
    c += "    " + readPin(*this, I_MEM_WE, "_we", 1) + "\n";
    c += "    " + readPin(*this, I_MEM_SEL, "_sel", 4) + "\n";
    c += "    " + readPin(*this, I_MEM_REQ, "_req", 1) + "\n";
    c += "    " + readPin(*this, I_MEM_LOCK, "_lock", 1) + "\n";
    c += "    " + genOutputWrite(O_WB_ADR, "_adr", 32) + "\n";
    c += "    " + genOutputWrite(O_WB_DAT, "_wdat", 32) + "\n";
    c += "    " + genOutputWrite(O_WB_WE, "_we", 1) + "\n";
    c += "    " + genOutputWrite(O_WB_SEL, "_sel", 4) + "\n";
    c += "    " + genOutputWrite(O_WB_STB, "_req", 1) + "\n";
    c += "    " + genOutputWrite(O_WB_CYC, "_lock", 1) + "\n";
    c += "}\n";
    return c;
}

std::string WbMasterAdapter::genFuncDef_comb() const {
    std::string c;
    c += std::format("// WbMasterAdapter comb {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_comb());
    // 应答方向：Wishbone 的 ack/rdat 组合透传给主设备，时序与原直连一致
    c += "    " + readPin(*this, I_WB_ACK, "_ack", 1) + "\n";
    c += "    " + readPin(*this, I_WB_DAT, "_rdat", 32) + "\n";
    c += "    " + genOutputWrite(O_MEM_ACK, "_ack", 1) + "\n";
    c += "    " + genOutputWrite(O_MEM_RDAT, "_rdat", 32) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> WbMasterAdapter::clone(const std::string &new_name) const {
    return std::make_unique<WbMasterAdapter>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"wb_master_adapter", {}, {"mem_adr", "mem_wdat", "mem_we", "mem_sel", "mem_req", "mem_lock", "wb_ack_i", "wb_dat_i"},
     {"wb_adr_o", "wb_dat_o", "wb_we_o", "wb_sel_o", "wb_stb_o", "wb_cyc_o", "mem_ack", "mem_rdat"}, dsc::ComponentCategory::Bus,
     "总线中立的存储器事务接口 → Wishbone 主设备；请求时序转发、应答组合透传"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::WbMasterAdapter>(n);
    });
} // namespace
