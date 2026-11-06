// wishbone_ic 测试夹具：主设备侧由测试驱动输入、读输出；从设备侧读转发信号、
// 用 s*_ack_i / s*_dat_i 模拟从设备的应答。互连是纯时序元件，输出比输入晚一拍。
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/components/platform/WishboneInterconnect.h>

namespace dsc_test {

inline uint32_t toU32(const std::vector<uint8_t> &v) {
    return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
}

inline void setU32(dsc::Circuit &c, dsc::Net *n, uint32_t v) {
    c.setWire(n->id(), {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)});
}

struct IcFixture {
    std::unique_ptr<dsc::Circuit> circuit = std::make_unique<dsc::Circuit>();
    dsc::WishboneInterconnect *ic = nullptr;
    dsc::Net *clk = nullptr;
    int M = 1;
    int N = 1;

    struct MBus {
        dsc::Net *adr = nullptr, *dat = nullptr, *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
        dsc::Net *dat_o = nullptr, *ack_o = nullptr;
    };
    struct SBus {
        dsc::Net *adr = nullptr, *dat = nullptr, *we = nullptr, *sel = nullptr, *stb = nullptr, *cyc = nullptr;
        dsc::Net *dat_i = nullptr, *ack_i = nullptr;
    };
    std::vector<MBus> mb;
    std::vector<SBus> sb;

    IcFixture(const std::string &segments, int masters) : M(masters) {
        auto p = std::make_unique<dsc::WishboneInterconnect>("ic0", segments, masters);
        ic = p.get();
        circuit->addComponent(std::move(p));
        N = ic->numSlaves();
        clk = circuit->createNet("clk");
        circuit->connect(ic, "clk", clk);

        for (int i = 0; i < M; i++) {
            const std::string pre = "m" + std::to_string(i);
            MBus b;
            b.adr = circuit->createNet(pre + "_adr");
            b.dat = circuit->createNet(pre + "_dat");
            b.we = circuit->createNet(pre + "_we");
            b.sel = circuit->createNet(pre + "_sel");
            b.stb = circuit->createNet(pre + "_stb");
            b.cyc = circuit->createNet(pre + "_cyc");
            b.dat_o = circuit->createNet(pre + "_dat_o");
            b.ack_o = circuit->createNet(pre + "_ack_o");
            circuit->connect(ic, pre + "_adr_i", b.adr);
            circuit->connect(ic, pre + "_dat_i", b.dat);
            circuit->connect(ic, pre + "_we_i", b.we);
            circuit->connect(ic, pre + "_sel_i", b.sel);
            circuit->connect(ic, pre + "_stb_i", b.stb);
            circuit->connect(ic, pre + "_cyc_i", b.cyc);
            circuit->connect(ic, pre + "_dat_o", b.dat_o);
            circuit->connect(ic, pre + "_ack_o", b.ack_o);
            mb.push_back(b);
        }

        for (int i = 0; i < N; i++) {
            const std::string pre = "s" + std::to_string(i);
            SBus b;
            b.adr = circuit->createNet(pre + "_adr");
            b.dat = circuit->createNet(pre + "_dat");
            b.we = circuit->createNet(pre + "_we");
            b.sel = circuit->createNet(pre + "_sel");
            b.stb = circuit->createNet(pre + "_stb");
            b.cyc = circuit->createNet(pre + "_cyc");
            b.dat_i = circuit->createNet(pre + "_dat_i");
            b.ack_i = circuit->createNet(pre + "_ack_i");
            circuit->connect(ic, pre + "_adr_o", b.adr);
            circuit->connect(ic, pre + "_dat_o", b.dat);
            circuit->connect(ic, pre + "_we_o", b.we);
            circuit->connect(ic, pre + "_sel_o", b.sel);
            circuit->connect(ic, pre + "_stb_o", b.stb);
            circuit->connect(ic, pre + "_cyc_o", b.cyc);
            circuit->connect(ic, pre + "_dat_i", b.dat_i);
            circuit->connect(ic, pre + "_ack_i", b.ack_i);
            sb.push_back(b);
        }
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, dsc::ErrorCode::OK) << err.message;
        circuit->init();
    }

    void tick(int n = 1) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk->id(), (uint8_t) 0);
            circuit->tick();
            circuit->setWire(clk->id(), (uint8_t) 1);
            circuit->tick();
        }
    }

    // ---- 主设备侧：测试扮演主设备 ----
    void setM(int mi, uint32_t adr, uint32_t dat, bool we, uint8_t sel, bool stb, bool cyc) {
        setU32(*circuit, mb[mi].adr, adr);
        setU32(*circuit, mb[mi].dat, dat);
        circuit->setWire(mb[mi].we->id(), (uint8_t) (we ? 1 : 0));
        circuit->setWire(mb[mi].sel->id(), sel);
        circuit->setWire(mb[mi].stb->id(), (uint8_t) (stb ? 1 : 0));
        circuit->setWire(mb[mi].cyc->id(), (uint8_t) (cyc ? 1 : 0));
    }

    void releaseM(int mi) {
        circuit->setWire(mb[mi].stb->id(), 0);
        circuit->setWire(mb[mi].cyc->id(), 0);
    }

    uint32_t mDat(int mi) const { return toU32(circuit->getWire(mb[mi].dat_o->id())); }
    uint8_t mAck(int mi) const { return circuit->getWire(mb[mi].ack_o->id())[0] & 1; }

    // ---- 从设备侧：测试扮演从设备 ----
    void setS(int si, uint32_t dat, bool ack) {
        setU32(*circuit, sb[si].dat_i, dat);
        circuit->setWire(sb[si].ack_i->id(), (uint8_t) (ack ? 1 : 0));
    }

    uint32_t sAdr(int si) const { return toU32(circuit->getWire(sb[si].adr->id())); }
    uint32_t sDat(int si) const { return toU32(circuit->getWire(sb[si].dat->id())); }
    uint8_t sWe(int si) const { return circuit->getWire(sb[si].we->id())[0] & 1; }
    uint8_t sSel(int si) const { return circuit->getWire(sb[si].sel->id())[0] & 0xF; }
    uint8_t sStb(int si) const { return circuit->getWire(sb[si].stb->id())[0] & 1; }
    uint8_t sCyc(int si) const { return circuit->getWire(sb[si].cyc->id())[0] & 1; }
};

} // namespace dsc_test
