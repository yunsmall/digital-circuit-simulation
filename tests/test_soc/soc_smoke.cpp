// 整机拓扑冒烟测试：CPU + 时钟发生器 + 多从设备互连 + RAM/CLINT/PLIC/UART，
// 全部在测试里手写，不依赖任何外部文件或镜像。
//
// 单元件测试覆盖不到这种"多主多从 + 时钟由元件产生 + 各设备同时在总线上的组合"，
// 整机级的问题（比如互连应答残留、设备地址译码错位）只有这种拓扑才能复现。
#include <gtest/gtest.h>
#include <dcs/Circuit.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <format>

using namespace dsc;

namespace {

// 4 个从设备：RAM / CLINT / PLIC / UART，地址段与各元件的寄存器布局对齐
const char *kSocJson = R"JSON({
  "nets": [
    "clk", "rst", "zero1", "zero8", "irq",
    "mem_adr", "mem_wdat", "mem_we", "mem_sel", "mem_req", "mem_lock", "mem_ack", "mem_rdat",
    "wb_adr", "wb_dat_o", "wb_dat_i", "wb_we", "wb_sel", "wb_stb", "wb_cyc", "wb_ack", "halted",
    "s0_adr", "s0_dat_o", "s0_dat_i", "s0_we", "s0_sel", "s0_stb", "s0_cyc", "s0_ack",
    "s1_adr", "s1_dat_o", "s1_dat_i", "s1_we", "s1_sel", "s1_stb", "s1_cyc", "s1_ack",
    "s2_adr", "s2_dat_o", "s2_dat_i", "s2_we", "s2_sel", "s2_stb", "s2_cyc", "s2_ack",
    "s3_adr", "s3_dat_o", "s3_dat_i", "s3_we", "s3_sel", "s3_stb", "s3_cyc", "s3_ack",
    "uart_tx", "uart_rx"
  ],
  "components": [
    {"type": "clkgen", "name": "clkgen", "params": {"high_ticks": "1", "low_ticks": "1"},
     "connections": {"clk": "clk"}},
    {"type": "constant", "name": "rst_zero", "params": {"bit_width": "1", "value": "0"},
     "connections": {"out": "rst"}},
    {"type": "constant", "name": "zero1", "params": {"bit_width": "1", "value": "0"},
     "connections": {"out": "zero1"}},
    {"type": "constant", "name": "zero8", "params": {"bit_width": "8", "value": "0"},
     "connections": {"out": "zero8"}},
    {"type": "constant", "name": "uart_idle", "params": {"bit_width": "1", "value": "1"},
     "connections": {"out": "uart_rx"}},
    {"type": "riscv_cpu", "name": "cpu", "params": {"xlen": "64"},
     "connections": {"clk": "clk", "rst": "rst", "mem_rdat": "mem_rdat", "mem_ack": "mem_ack",
                     "irq": "zero1", "mti": "zero1", "msi": "zero1", "seip": "zero1",
                     "mem_adr": "mem_adr", "mem_wdat": "mem_wdat", "mem_we": "mem_we",
                     "mem_sel": "mem_sel", "mem_req": "mem_req", "mem_lock": "mem_lock",
                     "halted": "halted"}},
    {"type": "wb_master_adapter", "name": "cpu_wb", "params": {},
     "connections": {"mem_adr": "mem_adr", "mem_wdat": "mem_wdat", "mem_we": "mem_we",
                     "mem_sel": "mem_sel", "mem_req": "mem_req", "mem_lock": "mem_lock",
                     "mem_ack": "mem_ack", "mem_rdat": "mem_rdat",
                     "wb_adr_o": "wb_adr", "wb_dat_o": "wb_dat_o", "wb_we_o": "wb_we",
                     "wb_sel_o": "wb_sel", "wb_stb_o": "wb_stb", "wb_cyc_o": "wb_cyc",
                     "wb_ack_i": "wb_ack", "wb_dat_i": "wb_dat_i"}},
    {"type": "wishbone_ic", "name": "ic",
     "params": {"segments": "2147483648:268435456,33554432:65536,201326592:67108864,268435456:256",
                "num_masters": "1"},
     "connections": {"m0_adr_i": "wb_adr", "m0_dat_i": "wb_dat_o", "m0_we_i": "wb_we",
                     "m0_sel_i": "wb_sel", "m0_stb_i": "wb_stb", "m0_cyc_i": "wb_cyc",
                     "m0_dat_o": "wb_dat_i", "m0_ack_o": "wb_ack",
                     "s0_adr_o": "s0_adr", "s0_dat_o": "s0_dat_o", "s0_we_o": "s0_we",
                     "s0_sel_o": "s0_sel", "s0_stb_o": "s0_stb", "s0_cyc_o": "s0_cyc",
                     "s0_dat_i": "s0_dat_i", "s0_ack_i": "s0_ack",
                     "clk": "clk",
                     "s1_adr_o": "s1_adr", "s1_dat_o": "s1_dat_o", "s1_we_o": "s1_we",
                     "s1_sel_o": "s1_sel", "s1_stb_o": "s1_stb", "s1_cyc_o": "s1_cyc",
                     "s1_dat_i": "s1_dat_i", "s1_ack_i": "s1_ack",
                     "s2_adr_o": "s2_adr", "s2_dat_o": "s2_dat_o", "s2_we_o": "s2_we",
                     "s2_sel_o": "s2_sel", "s2_stb_o": "s2_stb", "s2_cyc_o": "s2_cyc",
                     "s2_dat_i": "s2_dat_i", "s2_ack_i": "s2_ack",
                     "s3_adr_o": "s3_adr", "s3_dat_o": "s3_dat_o", "s3_we_o": "s3_we",
                     "s3_sel_o": "s3_sel", "s3_stb_o": "s3_stb", "s3_cyc_o": "s3_cyc",
                     "s3_dat_i": "s3_dat_i", "s3_ack_i": "s3_ack"}},
    {"type": "wishbone_mem", "name": "ram", "params": {"addr_width": "28", "data_width": "32"},
     "connections": {"clk": "clk", "wb_adr_i": "s0_adr", "wb_dat_i": "s0_dat_o", "wb_we_i": "s0_we",
                     "wb_sel_i": "s0_sel", "wb_stb_i": "s0_stb", "wb_cyc_i": "s0_cyc",
                     "wb_dat_o": "s0_dat_i", "wb_ack_o": "s0_ack"}},
    {"type": "clint", "name": "clint", "params": {},
     "connections": {"clk": "clk", "wb_adr_i": "s1_adr", "wb_dat_i": "s1_dat_o", "wb_we_i": "s1_we",
                     "wb_sel_i": "s1_sel", "wb_stb_i": "s1_stb", "wb_cyc_i": "s1_cyc",
                     "wb_dat_o": "s1_dat_i", "wb_ack_o": "s1_ack"}},
    {"type": "plic", "name": "plic", "params": {"num_sources": "8"},
     "connections": {"clk": "clk", "wb_adr_i": "s2_adr", "wb_dat_i": "s2_dat_o", "wb_we_i": "s2_we",
                     "wb_sel_i": "s2_sel", "wb_stb_i": "s2_stb", "wb_cyc_i": "s2_cyc",
                     "wb_dat_o": "s2_dat_i", "wb_ack_o": "s2_ack", "irq_src": "zero8"}},
    {"type": "uart", "name": "uart", "params": {"ns_per_period": "2", "baud": "125000000"},
     "connections": {"clk": "clk", "wb_adr_i": "s3_adr", "wb_dat_i": "s3_dat_o", "wb_we_i": "s3_we",
                     "wb_sel_i": "s3_sel", "wb_stb_i": "s3_stb", "wb_cyc_i": "s3_cyc",
                     "wb_dat_o": "s3_dat_i", "wb_ack_o": "s3_ack", "rx": "uart_rx",
                     "tx": "uart_tx"}}
  ]
})JSON";

// addi x1,x0,0x55 / sw x1,0x200(x0) / lw x2,0x200(x0) / jal x0,0
constexpr uint32_t kProgram[] = {0x05500093, 0x20102023, 0x20002103, 0x0000006F};

void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}
uint32_t r32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

} // namespace

// 跑一段"存-取"小程序：CPU 必须穿过互连把数据写进 RAM 再读回来。
// 这条路径把 CPU 总线口、互连主/从口、RAM 总线口全部串了起来
TEST(SocTest, CpuStoreLoadThroughInterconnect) {
    std::string err;
    auto circuit = Circuit::fromJson(kSocJson, err);
    ASSERT_NE(circuit, nullptr) << err;
    auto ce = circuit->compile();
    ASSERT_EQ(ce.code, ErrorCode::OK) << ce.message;
    circuit->init();

    WishboneMemory *mem = nullptr;
    RiscVCPU<64> *cpu = nullptr;
    for (auto &c: circuit->components()) {
        if (auto *m = dynamic_cast<WishboneMemory *>(c.get()))
            mem = m;
        if (auto *p = dynamic_cast<RiscVCPU<64> *>(c.get()))
            cpu = p;
    }
    ASSERT_NE(mem, nullptr);
    ASSERT_NE(cpu, nullptr);

    // 镜像链接在 0x80000000，存储器只有低 28 根地址线，高位是别名回来的
    for (size_t i = 0; i < std::size(kProgram); i++)
        w32(mem->writableMemPtr() + 4 * i, kProgram[i]);
    cpu->setPc(0x80000000);

    for (int i = 0; i < 2000; i++)
        circuit->tick();

    EXPECT_EQ(cpu->reg(1), 0x55u);
    // 小程序最后一条是 jal x0,0，PC 停在它上面就说明确实从 0x80000000 取指一路跑下来了
    EXPECT_EQ(cpu->pc(), 0x8000000Cu);
    EXPECT_EQ(cpu->reg(2), 0x55u) << "读回来的是存储器的值：互连的应答路径没把数据送回去";
    EXPECT_EQ(r32(mem->writableMemPtr() + 0x200), 0x55u);
}
