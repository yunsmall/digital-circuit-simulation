#pragma once
#include <dcs/Circuit.h>
#include <dcs/components/Arithmetic.h>
#include <dcs/components/DelayLine.h>
#include <dcs/components/Gates.h>
#include <dcs/components/Misc.h>
#include <dcs/components/Sequential.h>
#include <gtest/gtest.h>

// 测试各关键位宽的 AND 门（所有多输入门共用同一模板，以此覆盖）
inline void gateBitWidthTest(int bw) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in0"), *i1 = c.createNet("in1"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateAND>("g", 2, bw));
    c.connect(g, "in0", i0);
    c.connect(g, "in1", i1);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    std::vector<uint8_t> ones(16, 0xFF), pat(16, 0);
    for (int i = (bw + 7) / 8; i < 16; i++)
        ones[i] = 0;
    for (int i = 0; i < (bw + 7) / 8; i++)
        pat[i] = 0xAA;
    for (int i = (bw + 7) / 8; i < 16; i++)
        pat[i] = 0;
    int rem = bw % 8;
    if (rem != 0) {
        ones[(bw - 1) / 8] &= (1u << rem) - 1;
        pat[(bw - 1) / 8] &= (1u << rem) - 1;
    }
    c.setWire(i0->id(), ones);
    c.setWire(i1->id(), pat);
    c.tick();
    auto out = c.getWire(o->id());
    for (int i = 0; i < (bw + 7) / 8; i++)
        EXPECT_EQ(out[i], ones[i] & pat[i]) << "位宽=" << bw << " 字节=" << i;
    for (int i = (bw + 7) / 8; i < 16; i++)
        EXPECT_EQ(out[i], 0) << "位宽=" << bw << " 高位字节=" << i;
}

// NOT 门位宽测试：输入全 1 的指定宽度，输出应为按位取反（含掩码）
inline void notBitWidthTest(int bw) {
    dsc::Circuit c;
    auto *i0 = c.createNet("in"), *o = c.createNet("out");
    auto *g = c.addComponent(std::make_unique<dsc::GateNOT>("g", bw));
    c.connect(g, "in", i0);
    c.connect(g, "out", o);
    c.compile();
    c.init();
    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> ones(16, 0xFF);
    for (int i = bytes; i < 16; i++)
        ones[i] = 0;
    int rem = bw % 8;
    if (rem != 0)
        ones[bytes - 1] &= (1u << rem) - 1;
    c.setWire(i0->id(), ones);
    c.tick();
    auto out = c.getWire(o->id());
    for (int i = 0; i < bytes; i++) {
        uint8_t expected = ~ones[i];
        if (rem != 0 && i == bytes - 1)
            expected &= (1u << rem) - 1;
        EXPECT_EQ(out[i], expected) << "NOT bw=" << bw << " byte=" << i;
    }
    for (int i = bytes; i < 16; i++)
        EXPECT_EQ(out[i], 0) << "NOT bw=" << bw << " high=" << i;
}

// 加法器位宽测试
inline void adderTest(int bw, uint64_t a, uint64_t b, bool cin, uint64_t expSum, bool expCout) {
    dsc::Circuit c;
    auto *na = c.createNet("a"), *nb = c.createNet("b"), *nc = c.createNet("cin");
    auto *ns = c.createNet("sum"), *no = c.createNet("cout");
    auto *ad = c.addComponent(std::make_unique<dsc::Adder>("ad", bw));
    c.connect(ad, "a", na);
    c.connect(ad, "b", nb);
    c.connect(ad, "cin", nc);
    c.connect(ad, "sum", ns);
    c.connect(ad, "cout", no);
    c.compile();
    c.init();
    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> va(16, 0), vb(16, 0);
    for (int i = 0; i < bytes; i++) {
        va[i] = (a >> (i * 8)) & 0xFF;
        vb[i] = (b >> (i * 8)) & 0xFF;
    }
    c.setWire(na->id(), va);
    c.setWire(nb->id(), vb);
    c.setWire(nc->id(), {static_cast<uint8_t>(cin ? 1 : 0), 0});
    c.tick();
    auto sumOut = c.getWire(ns->id());
    uint64_t gotSum = 0;
    for (int i = 0; i < bytes; i++)
        gotSum |= (uint64_t) sumOut[i] << (i * 8);
    EXPECT_EQ(gotSum, expSum) << "bw=" << bw;
    EXPECT_EQ(c.getWire(no->id())[0] != 0, expCout) << "bw=" << bw;
}

// 减法器位宽测试
inline void subTest(int bw, uint64_t a, uint64_t b, bool bin, uint64_t expDiff, bool expBout) {
    dsc::Circuit c;
    auto *na = c.createNet("a"), *nb = c.createNet("b"), *nbin = c.createNet("bin");
    auto *nd = c.createNet("diff"), *nbout = c.createNet("bout");
    auto *sub = c.addComponent(std::make_unique<dsc::Subtractor>("sub", bw));
    c.connect(sub, "a", na);
    c.connect(sub, "b", nb);
    c.connect(sub, "bin", nbin);
    c.connect(sub, "diff", nd);
    c.connect(sub, "bout", nbout);
    c.compile();
    c.init();
    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> va(16, 0), vb(16, 0);
    for (int i = 0; i < bytes; i++) {
        va[i] = (a >> (i * 8)) & 0xFF;
        vb[i] = (b >> (i * 8)) & 0xFF;
    }
    c.setWire(na->id(), va);
    c.setWire(nb->id(), vb);
    c.setWire(nbin->id(), {static_cast<uint8_t>(bin ? 1 : 0), 0});
    c.tick();
    auto dOut = c.getWire(nd->id());
    uint64_t got = 0;
    for (int i = 0; i < bytes; i++)
        got |= (uint64_t) dOut[i] << (i * 8);
    EXPECT_EQ(got, expDiff) << "Sub bw=" << bw;
    EXPECT_EQ(c.getWire(nbout->id())[0] != 0, expBout) << "Sub bout bw=" << bw;
}

// 乘法器位宽测试（输出 prod_lo[bw] + prod_hi[bw] = 完整 2*bw 乘积）
inline void mulTest(int bw, uint64_t a, uint64_t b, uint64_t expLo, uint64_t expHi) {
    dsc::Circuit c;
    auto *na = c.createNet("a");
    auto *nb = c.createNet("b");
    auto *nlo = c.createNet("lo");
    auto *nhi = c.createNet("hi");
    auto *mul = c.addComponent(std::make_unique<dsc::Multiplier>("mul", bw));
    c.connect(mul, "a", na);
    c.connect(mul, "b", nb);
    c.connect(mul, "prod_lo", nlo);
    c.connect(mul, "prod_hi", nhi);
    c.compile();
    c.init();
    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> va(16, 0), vb(16, 0);
    for (int i = 0; i < bytes; i++) {
        va[i] = (a >> (i * 8)) & 0xFF;
        vb[i] = (b >> (i * 8)) & 0xFF;
    }
    c.setWire(na->id(), va);
    c.setWire(nb->id(), vb);
    c.tick();
    auto vlo = c.getWire(nlo->id());
    auto vhi = c.getWire(nhi->id());
    uint64_t gotLo = 0, gotHi = 0;
    for (int i = 0; i < bytes && i < 8; i++)
        gotLo |= (uint64_t) vlo[i] << (i * 8);
    for (int i = 0; i < bytes && i < 8; i++)
        gotHi |= (uint64_t) vhi[i] << (i * 8);
    EXPECT_EQ(gotLo, expLo) << "Mul bw=" << bw << " lo";
    EXPECT_EQ(gotHi, expHi) << "Mul bw=" << bw << " hi";
}

// 时序元件：DFF 位宽测试 helper
inline void dffBitWidthTest(int bw) {
    dsc::Circuit c;
    auto *d = c.createNet("d"), *clk = c.createNet("clk"), *q = c.createNet("q");
    auto *ff = c.addComponent(std::make_unique<dsc::DFlipFlop>("ff", bw));
    c.connect(ff, "d", d);
    c.connect(ff, "clk", clk);
    c.connect(ff, "q", q);
    c.compile();
    c.init();
    int bytes = (bw + 7) / 8;
    std::vector<uint8_t> val(16, 0);
    for (int i = 0; i < bytes; i++)
        val[i] = 0xAA;
    if (bw % 8 != 0)
        val[bytes - 1] &= (1u << (bw % 8)) - 1;
    c.setWire(d->id(), val);
    c.setWire(clk->id(), {0, 0});
    c.tick();
    c.setWire(clk->id(), {1, 0});
    c.tick();
    auto out = c.getWire(q->id());
    for (int i = 0; i < bytes; i++)
        EXPECT_EQ(out[i], val[i]) << "DFF bw=" << bw << " byte=" << i;
}
