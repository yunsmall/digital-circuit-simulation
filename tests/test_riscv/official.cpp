// 用官方 riscv-tests 用例交叉编译出的 ELF 直接驱动 CPU。
// 这些用例靠写 tohost 报告结果：写 1 表示通过，写 (用例号<<1)|1 表示失败。
// ELF 由 tests/riscv-official/build.sh 交叉编译，目录通过环境变量 DCS_RISCV_ELF_DIR 指定。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/platform/RiscVCPU.h>
#include "common/wb_cpu_link.h"
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/util/ElfLoader.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace dsc;

namespace {

struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU<32> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk_net = nullptr, *rst_net = nullptr, *irq_net = nullptr, *mti_net = nullptr, *msi_net = nullptr;
    Net *wb_adr = nullptr, *wb_dat_o = nullptr, *wb_dat_i = nullptr, *wb_we = nullptr;
    Net *wb_sel = nullptr, *wb_stb = nullptr, *wb_cyc = nullptr, *wb_ack = nullptr;
    Net *halted_net = nullptr;

    Fixture() {
        auto cpu_ptr = std::make_unique<RiscVCPU<32>>("cpu0");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));
        auto link = test::linkCpuToWb(*circuit, cpu);
        auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mem_ptr.get();
        circuit->addComponent(std::move(mem_ptr));

        clk_net = circuit->createNet("clk");
        rst_net = circuit->createNet("rst");
        irq_net = circuit->createNet("irq");
        mti_net = circuit->createNet("mti");
        msi_net = circuit->createNet("msi");
        wb_adr = link.adr;
        wb_dat_o = link.dat_o;
        wb_dat_i = link.dat_i;
        wb_we = link.we;
        wb_sel = link.sel;
        wb_stb = link.stb;
        wb_cyc = link.cyc;
        wb_ack = link.ack;
        halted_net = circuit->createNet("halted");

        circuit->connect(cpu, "clk", clk_net);
        circuit->connect(cpu, "rst", rst_net);
        circuit->connect(cpu, "irq", irq_net);
        circuit->connect(cpu, "mti", mti_net);
        circuit->connect(cpu, "msi", msi_net);
        circuit->connect(cpu, "halted", halted_net);

        circuit->connect(mem, "clk", clk_net);
        circuit->connect(mem, "wb_adr_i", wb_adr);
        circuit->connect(mem, "wb_dat_i", wb_dat_o);
        circuit->connect(mem, "wb_we_i", wb_we);
        circuit->connect(mem, "wb_sel_i", wb_sel);
        circuit->connect(mem, "wb_stb_i", wb_stb);
        circuit->connect(mem, "wb_cyc_i", wb_cyc);
        circuit->connect(mem, "wb_dat_o", wb_dat_i);
        circuit->connect(mem, "wb_ack_o", wb_ack);
    }

    bool compile() {
        auto err = circuit->compile();
        EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
        circuit->init();
        return err.code == ErrorCode::OK;
    }

    uint32_t peek(uint32_t addr) const {
        uint32_t v = 0;
        std::memcpy(&v, mem->writableMemPtr() + addr, 4);
        return v;
    }

    void cycle() {
        circuit->setWire(clk_net->id(), 0);
        circuit->tick();
        circuit->setWire(clk_net->id(), 1);
        circuit->tick();
    }
};

std::string elfDir() {
    if (const char *env = std::getenv("DCS_RISCV_ELF_DIR"))
        return env;
    return "tests/riscv-official/out";
}

// 返回 tohost 读到的值：1 表示通过，其余是失败编码；超时返回 0
uint32_t runElf(const std::string &path, std::string &err) {
    ElfImage img = loadElf32(path);
    if (!img.ok) {
        err = img.error;
        return 0;
    }
    if (img.tohost == 0) {
        err = "ELF 里没有 tohost 符号";
        return 0;
    }
    Fixture f;
    const size_t cap = 1u << 16; // WishboneMemory(name, 16, 32) 的地址空间
    // 测试可以链接在 DRAM_BASE(0x80000000) 这类高地址：存储器只有低 addr_width 根
    // 地址线，高位在硬件上不存在、自然别名回来，装载偏移同样取容量的低 cap 位
    const size_t load_off = static_cast<size_t>(img.base) & (cap - 1);
    if (load_off + img.data.size() > cap) {
        err = "映像超出存储器容量";
        return 0;
    }
    std::memcpy(f.mem->writableMemPtr() + load_off, img.data.data(), img.data.size());
    if (!f.compile()) {
        err = "compile 失败";
        return 0;
    }
    f.circuit->setWire(f.clk_net->id(), 0);
    f.circuit->setWire(f.rst_net->id(), 1);
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), 0);
    // 链接在高地址的镜像要从自己的入口开始执行，否则 PC 相对寻址全按 0 基址算
    f.cpu->setPc((uint32_t) img.entry);

    for (int i = 0; i < 200000; ++i) {
        f.cycle();
        uint32_t v = f.peek(static_cast<uint32_t>(img.tohost) & (uint32_t)(cap - 1));
        if (v != 0)
            return v;
    }
    err = "超时，tohost 始终为 0";
    return 0;
}

} // namespace

TEST(RiscVOfficial, AllElf) {
    namespace fs = std::filesystem;
    const std::string dir = elfDir();
    // 环境变量是 CMake 设的绝对路径（构建目录里跑 ctest 就是这种情况）：目录缺失
    // 说明 ELF 没编出来，不能算通过，否则 CI 上会静默跳过、结果仍是绿的
    const bool configured = std::getenv("DCS_RISCV_ELF_DIR") != nullptr;
    if (!fs::is_directory(dir)) {
        if (configured)
            GTEST_FAIL() << "官方测试 ELF 目录不存在: " << dir << "（先跑 tests/riscv-official/build.sh）";
        GTEST_SKIP() << "找不到官方测试 ELF 目录: " << dir << "（先跑 tests/riscv-official/build.sh）";
    }
    std::vector<fs::path> elfs;
    for (const auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".elf")
            elfs.push_back(e.path());
    if (elfs.empty()) {
        if (configured)
            GTEST_FAIL() << "官方测试 ELF 目录是空的: " << dir << "（build.sh 可能编译失败）";
        GTEST_SKIP() << "目录里没有 .elf: " << dir;
    }
    std::sort(elfs.begin(), elfs.end());

    // 每个用例都搭一份独立电路，JIT 上下文是每实例独立的（编译器目标初始化已 call_once），
    // 所以可以并行跑。默认 4 路：ctest 本身还会并行跑多个测试目标，再高会互相抢核
    unsigned nthreads = 4;
    if (const char *e = std::getenv("DCS_TEST_THREADS")) nthreads = (unsigned) std::max(1, std::atoi(e));
    std::vector<std::string> errs(elfs.size());
    std::vector<uint32_t> vals(elfs.size(), 0);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= elfs.size())
                break;
            vals[i] = runElf(elfs[i].string(), errs[i]);
        }
    };
    if (nthreads <= 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);
        for (auto &t : pool) t.join();
    }
    // 汇总按文件名顺序打印，结果与并行调度无关
    for (size_t i = 0; i < elfs.size(); i++) {
        if (!errs[i].empty()) {
            ADD_FAILURE() << elfs[i].filename().string() << ": " << errs[i];
            continue;
        }
        if (vals[i] != 1) {
            ADD_FAILURE() << elfs[i].filename().string() << ": 失败编码 " << vals[i]
                          << "（用例号 " << (vals[i] >> 1) << "）";
        }
    }
}
