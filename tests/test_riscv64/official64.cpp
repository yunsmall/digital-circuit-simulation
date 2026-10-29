// RV64 实例的官方 riscv-tests 用例：与 RV32 那套完全独立（夹具、ELF 目录、
// 环境变量都是自己的），避免两个位宽混在同一个测试目标里互相干扰。
// 这些用例靠写 tohost 报告结果：写 1 表示通过，写 (用例号<<1)|1 表示失败。
// ELF 由 tests/riscv-official/build_rv64.sh 交叉编译，目录通过环境变量 DCS_RISCV64_ELF_DIR 指定。
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/WishboneMemory.h>
#include <dcs/util/ElfLoader.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace dsc;

namespace {

struct Fixture {
    std::unique_ptr<Circuit> circuit = std::make_unique<Circuit>();
    RiscVCPU<64> *cpu = nullptr;
    WishboneMemory *mem = nullptr;
    Net *clk_net = nullptr, *rst_net = nullptr, *irq_net = nullptr, *mti_net = nullptr, *msi_net = nullptr;
    Net *wb_adr = nullptr, *wb_dat_o = nullptr, *wb_dat_i = nullptr, *wb_we = nullptr;
    Net *wb_sel = nullptr, *wb_stb = nullptr, *wb_cyc = nullptr, *wb_ack = nullptr;
    Net *halted_net = nullptr;

    Fixture() {
        auto cpu_ptr = std::make_unique<RiscVCPU<64>>("cpu0");
        cpu = cpu_ptr.get();
        circuit->addComponent(std::move(cpu_ptr));
        auto mem_ptr = std::make_unique<WishboneMemory>("mem0", 16, 32);
        mem = mem_ptr.get();
        circuit->addComponent(std::move(mem_ptr));

        clk_net = circuit->createNet("clk");
        rst_net = circuit->createNet("rst");
        irq_net = circuit->createNet("irq");
        mti_net = circuit->createNet("mti");
        msi_net = circuit->createNet("msi");
        wb_adr = circuit->createNet("wb_adr");
        wb_dat_o = circuit->createNet("wb_dat_o");
        wb_dat_i = circuit->createNet("wb_dat_i");
        wb_we = circuit->createNet("wb_we");
        wb_sel = circuit->createNet("wb_sel");
        wb_stb = circuit->createNet("wb_stb");
        wb_cyc = circuit->createNet("wb_cyc");
        wb_ack = circuit->createNet("wb_ack");
        halted_net = circuit->createNet("halted");

        circuit->connect(cpu, "clk", clk_net);
        circuit->connect(cpu, "rst", rst_net);
        circuit->connect(cpu, "irq", irq_net);
        circuit->connect(cpu, "mti", mti_net);
        circuit->connect(cpu, "msi", msi_net);
        circuit->connect(cpu, "wb_dat_i", wb_dat_i);
        circuit->connect(cpu, "wb_ack_i", wb_ack);
        circuit->connect(cpu, "wb_adr_o", wb_adr);
        circuit->connect(cpu, "wb_dat_o", wb_dat_o);
        circuit->connect(cpu, "wb_we_o", wb_we);
        circuit->connect(cpu, "wb_sel_o", wb_sel);
        circuit->connect(cpu, "wb_stb_o", wb_stb);
        circuit->connect(cpu, "wb_cyc_o", wb_cyc);
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
        circuit->setWire(clk_net->id(), {0});
        circuit->tick();
        circuit->setWire(clk_net->id(), {1});
        circuit->tick();
    }
};

std::string elfDir() {
    if (const char *env = std::getenv("DCS_RISCV64_ELF_DIR"))
        return env;
    return "tests/riscv-official/out64";
}

// 返回 tohost 读到的值：1 表示通过，其余是失败编码；超时返回 0
uint32_t runElf(const std::string &path, std::string &err) {
    ElfImage img = loadElf64(path);
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
    f.circuit->setWire(f.clk_net->id(), {0});
    f.circuit->setWire(f.rst_net->id(), {1});
    f.circuit->tick();
    f.circuit->setWire(f.rst_net->id(), {0});
    // 链接在高地址的镜像要从自己的入口开始执行，否则 PC 相对寻址全按 0 基址算
    f.cpu->setPc((uint64_t) img.entry);

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

TEST(RiscV64Official, AllElf) {
    namespace fs = std::filesystem;
    const std::string dir = elfDir();
    if (!fs::is_directory(dir)) {
        GTEST_SKIP() << "找不到 RV64 官方测试 ELF 目录: " << dir << "（先跑 tests/riscv-official/build_rv64.sh）";
    }
    std::vector<fs::path> elfs;
    for (const auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".elf")
            elfs.push_back(e.path());
    if (elfs.empty())
        GTEST_SKIP() << "目录里没有 .elf: " << dir;
    std::sort(elfs.begin(), elfs.end());

    int skipped = 0;
    for (const auto &p : elfs) {
        std::string err;
        uint32_t v = runElf(p.string(), err);
        if (!err.empty()) {
            ADD_FAILURE() << p.filename().string() << ": " << err;
            continue;
        }
        if (v != 1) {
            ADD_FAILURE() << p.filename().string() << ": 失败编码 " << v
                          << "（用例号 " << (v >> 1) << "）";
        }
    }
}
