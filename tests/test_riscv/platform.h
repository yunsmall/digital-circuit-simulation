// RISC-V 测试共用的最小平台：CPU + WishboneMemory + 引脚连线
#pragma once
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/memory/WishboneMemory.h>
#include "common/wb_cpu_link.h"
#include <cstring>
#include <memory>
#include <vector>
#include <cstdint>

using namespace dsc;

struct PrivTestFixture {
    std::unique_ptr<Circuit> circuit;
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk_net, *rst_net, *irq_net, *mti_net, *msi_net;
    test::WbCpuLink link;
    Net *wb_adr, *wb_dat_o, *wb_dat_i, *wb_we, *wb_sel, *wb_stb, *wb_cyc, *wb_ack;
    Net *halted_net;

    PrivTestFixture() {
        circuit = std::make_unique<Circuit>();
        auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));
        // 总线中立接口 → Wishbone；适配器紧跟 CPU 加入，请求才能同拍转发
        link = test::linkCpuToWb(*circuit, cpu);
        auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mem_ptr.get();
        circuit->addComponent(std::move(mem_ptr));
        clk_net = circuit->createNet("clk"); rst_net = circuit->createNet("rst");
        irq_net = circuit->createNet("irq");
        mti_net = circuit->createNet("mti");
        msi_net = circuit->createNet("msi");
        wb_adr = link.adr; wb_dat_o = link.dat_o; wb_dat_i = link.dat_i;
        wb_we = link.we; wb_sel = link.sel; wb_stb = link.stb;
        wb_cyc = link.cyc; wb_ack = link.ack;
        halted_net = circuit->createNet("halted");
        circuit->connect(cpu, "clk", clk_net); circuit->connect(cpu, "rst", rst_net);
        circuit->connect(cpu, "irq", irq_net); circuit->connect(cpu, "mti", mti_net);
        circuit->connect(cpu, "msi", msi_net);
        circuit->connect(cpu, "halted", halted_net);
        circuit->connect(mem, "clk", clk_net); circuit->connect(mem, "wb_adr_i", wb_adr);
        circuit->connect(mem, "wb_dat_i", wb_dat_o); circuit->connect(mem, "wb_we_i", wb_we);
        circuit->connect(mem, "wb_sel_i", wb_sel); circuit->connect(mem, "wb_stb_i", wb_stb);
        circuit->connect(mem, "wb_cyc_i", wb_cyc); circuit->connect(mem, "wb_dat_o", wb_dat_i);
        circuit->connect(mem, "wb_ack_o", wb_ack);
    }

    void loadProgram(const std::vector<uint32_t> &insts) {
        uint8_t *m = mem->writableMemPtr();
        for (size_t i = 0; i < insts.size(); i++) {
            uint32_t inst = insts[i];
            m[i * 4 + 0] = (uint8_t)(inst & 0xFF);
            m[i * 4 + 1] = (uint8_t)((inst >> 8) & 0xFF);
            m[i * 4 + 2] = (uint8_t)((inst >> 16) & 0xFF);
            m[i * 4 + 3] = (uint8_t)((inst >> 24) & 0xFF);
        }
    }

    void compile() {
        auto err = circuit->compile();
        ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
    }

    void runCycles(int n) {
        for (int i = 0; i < n; i++) {
            circuit->setWire(clk_net->id(), 0);
            circuit->tick();
            circuit->setWire(clk_net->id(), 1);
            circuit->tick();
        }
    }

    void reset() {
        circuit->setWire(clk_net->id(), 0);
        circuit->setWire(rst_net->id(), 1);
        circuit->setWire(irq_net->id(), 0);
        circuit->tick();
        circuit->setWire(rst_net->id(), 0);
    }
};
