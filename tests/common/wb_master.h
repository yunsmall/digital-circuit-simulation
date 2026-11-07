// Wishbone 从设备的通用测试台。
// 每个从设备测试都要重复"建网表、连引脚、assert stb、轮询 ack、撤销 stb"这一套，
// 而且时序细节写错就会得到假绿（stb 拉低不够拍数、把残留 ack 当新 ack）。
// 这里把主设备那一侧固定下来，各测试只需要关心被测评设备的行为。
#pragma once
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/Component.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace dsc::test {

// 模拟一个 Wishbone 主设备，驱动一根从设备总线。
// 所有信号都过 _w[]：主设备这一拍写进去的值从设备下一拍才看到，
// 从设备的 ack/dat 同样晚一拍回来——与接真实 CPU 时的时序一致。
class WbMaster {
public:
    explicit WbMaster(Circuit &c) : _c(c) {
        clk = c.createNet("clk");
        rst = c.createNet("rst");
        adr = c.createNet("wb_adr");
        dat_o = c.createNet("wb_dat_o");
        we = c.createNet("wb_we");
        sel = c.createNet("wb_sel");
        stb = c.createNet("wb_stb");
        cyc = c.createNet("wb_cyc");
        dat_i = c.createNet("wb_dat_i");
        ack = c.createNet("wb_ack");
    }

    // 从设备不一定有 rst 引脚（WishboneMemory 就没有），按需连接
    static bool hasInput(Component *c, const char *name) {
        for (const auto &p : c->inputs()) if (p->name() == name) return true;
        return false;
    }
    void attach(Component *slave) {
        _c.connect(slave, "clk", clk);
        if (hasInput(slave, "rst")) _c.connect(slave, "rst", rst);
        _c.connect(slave, "wb_adr_i", adr);
        _c.connect(slave, "wb_dat_i", dat_o);
        _c.connect(slave, "wb_we_i", we);
        _c.connect(slave, "wb_sel_i", sel);
        _c.connect(slave, "wb_stb_i", stb);
        _c.connect(slave, "wb_cyc_i", cyc);
        _c.connect(slave, "wb_dat_o", dat_i);
        _c.connect(slave, "wb_ack_o", ack);
    }

    void finish() {
        _c.compile();
        _c.init();
        _c.setWire(clk->id(), (uint8_t)0);
        _c.setWire(rst->id(), (uint8_t)1);
        _c.setWire(stb->id(), (uint8_t)0);
        _c.setWire(cyc->id(), (uint8_t)0);
        _c.tick();
        _c.setWire(rst->id(), (uint8_t)0);
        _c.tick();
    }

    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            _c.setWire(clk->id(), (uint8_t)0);
            _c.tick();
            _c.setWire(clk->id(), (uint8_t)1);
            _c.tick();
        }
    }

    // 驱动总线电平，保持到下一次 drive
    // setWire 收 byte 数组，窄位宽的信号只填最低字节
    void drive(uint32_t a, bool w, uint32_t d, uint8_t s, bool st, bool cy) {
        _c.setWire(adr->id(), { (uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF),
                                (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 24) & 0xFF) });
        _c.setWire(we->id(), (uint8_t)(w ? 1 : 0));
        _c.setWire(dat_o->id(), { (uint8_t)(d & 0xFF), (uint8_t)((d >> 8) & 0xFF),
                                  (uint8_t)((d >> 16) & 0xFF), (uint8_t)((d >> 24) & 0xFF) });
        _c.setWire(sel->id(), (uint8_t)(s & 0x0F));
        _c.setWire(stb->id(), (uint8_t)(st ? 1 : 0));
        _c.setWire(cyc->id(), (uint8_t)(cy ? 1 : 0));
    }

    // 64 位端口版本：DAT/SEL 都要写满整根线，不能只填低字节
    void drive64(uint32_t a, bool w, uint64_t d, uint8_t s, bool st, bool cy) {
        _c.setWire(adr->id(), { (uint8_t)(a & 0xFF), (uint8_t)((a >> 8) & 0xFF),
                                (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 24) & 0xFF) });
        _c.setWire(we->id(), (uint8_t)(w ? 1 : 0));
        std::vector<uint8_t> db(8);
        for (int i = 0; i < 8; ++i) db[i] = (uint8_t)((d >> (8 * i)) & 0xFF);
        _c.setWire(dat_o->id(), db);
        _c.setWire(sel->id(), (uint8_t)s);
        _c.setWire(stb->id(), (uint8_t)(st ? 1 : 0));
        _c.setWire(cyc->id(), (uint8_t)(cy ? 1 : 0));
    }

    // getWire 返回字节数组（小端），拼回 32 位
    uint32_t readData() const {
        auto v = _c.getWire(dat_i->id());
        uint32_t r = 0;
        for (size_t i = 0; i < v.size() && i < 4; ++i) r |= (uint32_t)v[i] << (8 * i);
        return r;
    }
    bool ackHigh() const {
        auto v = _c.getWire(ack->id());
        return !v.empty() && v[0] != 0;
    }
    uint64_t readData64() const {
        auto v = _c.getWire(dat_i->id());
        uint64_t r = 0;
        for (size_t i = 0; i < v.size() && i < 8; ++i) r |= (uint64_t)v[i] << (8 * i);
        return r;
    }

    // 一笔完整事务：拉高 stb、等 ack、采样、拉低 stb 并让从设备忘掉这笔。
    // 返回 false 表示超时（从设备没给 ack）。
    bool transact(uint32_t a, bool w, uint32_t d, uint8_t s,
                  uint32_t *out = nullptr, int max_cycles = 64) {
        drive(a, w, d, s, true, true);
        bool got = false;
        for (int i = 0; i < max_cycles; ++i) {
            tick();
            if (ackHigh()) { got = true; break; }
        }
        if (got && out) *out = readData();
        drive(a, w, d, s, false, false);
        tick(3);   // 让从设备看到 stb 撤销并清掉 ack_pending，再开下一笔
        return got;
    }

    bool transact64(uint32_t a, bool w, uint64_t d, uint8_t s,
                    uint64_t *out = nullptr, int max_cycles = 64) {
        drive64(a, w, d, s, true, true);
        bool got = false;
        for (int i = 0; i < max_cycles; ++i) {
            tick();
            if (ackHigh()) { got = true; break; }
        }
        if (got && out) *out = readData64();
        drive64(a, w, d, s, false, false);
        tick(3);
        return got;
    }

    Circuit &_c;
    Net *clk, *rst;
    Net *adr, *dat_o, *we, *sel, *stb, *cyc, *dat_i, *ack;
};

} // namespace dsc::test
