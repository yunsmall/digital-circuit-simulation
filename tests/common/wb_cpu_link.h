#pragma once
#include <dcs/Circuit.h>
#include <dcs/components/platform/WbMasterAdapter.h>
#include <memory>
#include <string>

namespace dsc::test {

// CPU 的总线中立接口到 Wishbone 的接线结果：wb_* 给从设备/互连用
struct WbCpuLink {
    WbMasterAdapter *adapter = nullptr;
    Net *adr = nullptr, *dat_o = nullptr, *dat_i = nullptr, *we = nullptr;
    Net *sel = nullptr, *stb = nullptr, *cyc = nullptr, *ack = nullptr;
};

// 建好 CPU 的 mem_* 线网和 wb_master_adapter，并把 wb 侧的 8 根线网暴露出来。
// 适配器必须紧跟 CPU 加入电路（阶段 3 顺序决定请求同拍可见）
inline WbCpuLink linkCpuToWb(Circuit &c, Component *cpu, const std::string &prefix = "") {
    auto mk = [&](const char *name) { return c.createNet(prefix + name); };
    WbCpuLink l;
    l.adr = mk("wb_adr");
    l.dat_o = mk("wb_dat_o");
    l.dat_i = mk("wb_dat_i");
    l.we = mk("wb_we");
    l.sel = mk("wb_sel");
    l.stb = mk("wb_stb");
    l.cyc = mk("wb_cyc");
    l.ack = mk("wb_ack");
    Net *m_adr = mk("mem_adr"), *m_wdat = mk("mem_wdat"), *m_we = mk("mem_we"), *m_sel = mk("mem_sel");
    Net *m_req = mk("mem_req"), *m_lock = mk("mem_lock"), *m_ack = mk("mem_ack"), *m_rdat = mk("mem_rdat");

    auto up = std::make_unique<WbMasterAdapter>(prefix + "cpu_wb");
    l.adapter = up.get();
    c.addComponent(std::move(up));

    c.connect(cpu, "mem_adr", m_adr);
    c.connect(cpu, "mem_wdat", m_wdat);
    c.connect(cpu, "mem_we", m_we);
    c.connect(cpu, "mem_sel", m_sel);
    c.connect(cpu, "mem_req", m_req);
    c.connect(cpu, "mem_lock", m_lock);
    c.connect(cpu, "mem_ack", m_ack);
    c.connect(cpu, "mem_rdat", m_rdat);

    c.connect(l.adapter, "mem_adr", m_adr);
    c.connect(l.adapter, "mem_wdat", m_wdat);
    c.connect(l.adapter, "mem_we", m_we);
    c.connect(l.adapter, "mem_sel", m_sel);
    c.connect(l.adapter, "mem_req", m_req);
    c.connect(l.adapter, "mem_lock", m_lock);
    c.connect(l.adapter, "mem_ack", m_ack);
    c.connect(l.adapter, "mem_rdat", m_rdat);
    c.connect(l.adapter, "wb_adr_o", l.adr);
    c.connect(l.adapter, "wb_dat_o", l.dat_o);
    c.connect(l.adapter, "wb_we_o", l.we);
    c.connect(l.adapter, "wb_sel_o", l.sel);
    c.connect(l.adapter, "wb_stb_o", l.stb);
    c.connect(l.adapter, "wb_cyc_o", l.cyc);
    c.connect(l.adapter, "wb_ack_i", l.ack);
    c.connect(l.adapter, "wb_dat_i", l.dat_i);
    return l;
}

} // namespace dsc::test
