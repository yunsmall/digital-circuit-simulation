// 多场景性能基准：加载 benchmarks/out 下的 ELF，在固定拍数下测量每秒仿真拍数。
// 覆盖整数 ALU、访存、分支、乘除、双精度浮点、混合负载六种负载，
// 都用同一个 CPU + WishboneMemory 电路，测的是电路仿真的吞吐而不是某个用例的完成时间。
//
// 用法: dcs_bench [--ticks N] [--repeat R] [--dir 目录] [elf ...]
//       默认跑 benchmarks/out 下全部 ELF，每个 4000 万拍，重复 3 次取最小值
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/Gates.h>
#include <dcs/components/RiscVCPU.h>
#include <dcs/components/Sequential.h>
#include <dcs/components/WishboneMemory.h>
#include <dcs/util/ElfLoader.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
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
    double compile_ms = 0;   // 电路构建 + JIT 编译耗时
    uint32_t tohost = 0;     // tohost 折叠到存储器内的地址，用来判断程序跑完

    // use_clkgen：时钟由电路内的 ClockGen 元件产生（每 tick 翻转），
    // 对应"电路自己走时钟"的用法；默认由 tickCycles 在外部驱动
    explicit Fixture(bool use_clkgen = false) {
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

        if (use_clkgen) {
            auto cg = std::make_unique<ClockGen>("clkgen0", 1, 1);
            auto *p = circuit->addComponent(std::move(cg));
            circuit->connect(p, "clk", clk_net);
        }
    }

    uint32_t peek(uint32_t addr) const {
        uint32_t v = 0;
        std::memcpy(&v, mem->writableMemPtr() + addr, 4);
        return v;
    }

    bool load(const std::string &path, std::string &err) {
        auto img = loadElf(path);
        if (!img.ok) { err = img.error; return false; }
        const size_t cap = mem->memSize();
        size_t off = (size_t) img.base & (cap - 1);
        const uint8_t *src = img.data.data();
        size_t remain = img.data.size();
        while (remain > 0) {
            size_t chunk = (std::min)(remain, cap - off);
            std::memcpy(mem->writableMemPtr() + off, src, chunk);
            src += chunk; remain -= chunk; off = (off + chunk) & (cap - 1);
        }
        auto c0 = std::chrono::steady_clock::now();
        tohost = static_cast<uint32_t>(img.tohost & (cap - 1));
        auto err2 = circuit->compile();
        compile_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        if (err2.code != ErrorCode::OK) { err = err2.message; return false; }
        circuit->init();
        cpu->setPc(img.entry);
        return true;
    }

    void reset(bool drive_clk = true) {
        if (drive_clk)
            circuit->setWire(clk_net->id(), 0);
        circuit->setWire(rst_net->id(), 1);
        circuit->tick();
        circuit->setWire(rst_net->id(), 0);
    }
};

struct Result {
    std::string name;
    double compile_ms = 0;   // 电路构建 + JIT 编译的耗时，和仿真速度分开看
    double seconds = 0;
    long long ticks = 0;
    long long done_cycles = 0;   // 跑到 tohost 写 1 所需的周期数；0 表示没跑完
};

// 同一个 CPU 电路，改用 ClockGen 元件产生时钟：每拍 tick 一次。
// 与 tickCycles 的外部驱动方式对比，能看出"多跑一个元件函数"的代价
void runClockGenCase(const std::string &path, long long ticks, int repeat, std::vector<Result> &out) {
    Result r;
    r.name = std::filesystem::path(path).stem().string() + "_clkgen";
    Fixture f(true);
    std::string err;
    if (!f.load(path, err)) {
        std::fprintf(stderr, "[%s] 装载失败: %s\n", r.name.c_str(), err.c_str());
        return;
    }
    r.compile_ms = f.compile_ms;
    double best = 1e30;
    for (int rep = 0; rep < repeat; rep++) {
        f.reset(false);
        if (f.tohost)
            std::memset(f.mem->writableMemPtr() + (f.tohost & 0xFFFFu), 0, 4);
        long long done = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < ticks; i++) {
            f.circuit->tick();
            // 每 2048 个 tick（1024 个周期）查一次 tohost
            if (f.tohost && (i & 2047) == 0 && f.peek(f.tohost & 0xFFFFu) != 0) {
                done = i / 2;
                break;
            }
        }
        best = (std::min)(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (rep == 0) r.done_cycles = done;
    }
    r.seconds = best;
    // 提前完成时只用实际跑过的 tick 数算速率，否则分母虚高
    r.ticks = r.done_cycles > 0 ? r.done_cycles * 2 : ticks;
    out.push_back(r);
}

// 纯电路场景：不加载 ELF，直接搭电路，测框架调度本身每拍的开销
std::unique_ptr<Circuit> makeGateChain(int levels) {
    auto c = std::make_unique<Circuit>();
    Net *prev = c->createNet("in0");
    for (int i = 0; i < levels; i++) {
        auto g = std::make_unique<UnaryGate>("not" + std::to_string(i), 32, true);
        auto *gp = c->addComponent(std::move(g));
        c->connect(gp, "in", prev);
        Net *out = c->createNet("n" + std::to_string(i));
        c->connect(gp, "out", out);
        prev = out;
    }
    return c;
}

std::unique_ptr<Circuit> makeDffBank(int n, Net *&clk_out) {
    auto c = std::make_unique<Circuit>();
    clk_out = c->createNet("clk");
    Net *din = c->createNet("din");
    for (int i = 0; i < n; i++) {
        auto ff = std::make_unique<DFlipFlop>("ff" + std::to_string(i), 32);
        auto *p = c->addComponent(std::move(ff));
        c->connect(p, "d", din);
        c->connect(p, "clk", clk_out);
        Net *q = c->createNet("q" + std::to_string(i));
        c->connect(p, "q", q);
    }
    return c;
}

// 跑固定拍数并计时；best-of-repeat，避免一次调度抖动影响结果
double measure(Circuit &c, Net *clk, long long cycles, int repeat) {
    double best = 1e30;
    for (int rep = 0; rep < repeat; rep++) {
        c.init();
        auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < cycles; i++)
            c.tickCycle(clk->id());
        auto t1 = std::chrono::steady_clock::now();
        best = (std::min)(best, std::chrono::duration<double>(t1 - t0).count());
    }
    return best;
}

void runOne(const std::string &path, long long ticks, int repeat, std::vector<Result> &out) {
    Result r;
    r.name = std::filesystem::path(path).stem().string();
    double best = 1e30;
    // 电路只搭一次、只编译一次：多轮测量的是同一份 JIT 代码，
    // 编译耗时单独记录，不要混进仿真速度里
    Fixture f;
    std::string err;
    if (!f.load(path, err)) {
        std::fprintf(stderr, "[%s] 装载失败: %s\n", r.name.c_str(), err.c_str());
        return;
    }
    r.compile_ms = f.compile_ms;
    for (int rep = 0; rep < repeat; rep++) {
        f.reset();
        // 每轮开始前清掉上一轮写的 tohost，否则第二轮第一拍就会被判成"已完成"
        if (f.tohost)
            std::memset(f.mem->writableMemPtr() + (f.tohost & 0xFFFFu), 0, 4);
        auto t0 = std::chrono::steady_clock::now();
        long long i = 0;
        long long done = 0;
        for (; i < ticks; i++) {
            f.circuit->tickCycle(f.clk_net->id());
            // 每 1024 个周期查一次 tohost：检查本身要读存储器，别每拍都做
            if (f.tohost && (i & 1023) == 0 && f.peek(f.tohost & 0xFFFFu) != 0) {
                done = i;
                break;
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        best = (std::min)(best, sec);
        // 每个时钟周期做两次 circuit_tick（低电平拍 + 高电平拍）
        if (rep == 0) {
            r.ticks = i * 2;
            r.done_cycles = done;
            if (f.cpu && done == 0)
                std::fprintf(stderr, "  [%s] 未完成: pc=0x%llx mcause=%u mepc=0x%x mtval=0x%x\n", r.name.c_str(),
                             (unsigned long long) f.cpu->pc(), f.cpu->mcause(), f.cpu->mepc(), f.cpu->mtval());
            if (f.cpu)
                std::fprintf(stderr, "  [%s] icache 命中 %llu / 缺失 %llu\n", r.name.c_str(),
                             (unsigned long long) f.cpu->icacheHits(), (unsigned long long) f.cpu->icacheMisses());
            if (f.cpu) {
                std::fprintf(stderr, "  最近缺失:");
                for (int k = 0; k < 8; k++) std::fprintf(stderr, " 0x%x", f.cpu->icacheRecentMisses()[k]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    r.seconds = best;
    out.push_back(r);
}

} // namespace

int main(int argc, char **argv) {
    long long ticks = 40000000;
    int repeat = 3;
    std::string dir = "benchmarks/out";
    std::vector<std::string> files;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc) ticks = std::atoll(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else files.push_back(a);
    }
    if (const char *e = std::getenv("DCS_BENCH_DIR")) dir = e;
    const std::vector<std::string> explicit_list = files;

    namespace fs = std::filesystem;
    if (files.empty()) {
        if (!fs::is_directory(dir)) {
            std::fprintf(stderr, "找不到基准目录 %s（先跑 bash benchmarks/build.sh）\n", dir.c_str());
            return 1;
        }
        for (const auto &e : fs::directory_iterator(dir))
            if (e.path().extension() == ".elf")
                files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
    }
    if (files.empty()) {
        std::fprintf(stderr, "没有可跑的 ELF\n");
        return 1;
    }

    std::vector<Result> results;
    for (const auto &f : files) runOne(f, ticks, repeat, results);

    // 用第一个 ELF 再跑一遍"电路内部时钟"的对比场景
    if (!files.empty() && !explicit_list.empty())
        runClockGenCase(files.front(), ticks, repeat, results);
    else if (!files.empty() && explicit_list.empty())
        runClockGenCase(files.front(), ticks, repeat, results);

    // 显式指定 ELF 时只跑这些，方便单场景剖析
    const bool explicit_files = !explicit_list.empty();
    // 纯电路场景：没有 CPU 取指，反映调度器和代码生成本身的每拍开销
    if (!explicit_files) {
        auto c = makeGateChain(256);
        Net *clk = c->createNet("clk"); // 组合链不需要时钟，但 measure 要一个可驱动的线网
        auto e = c->compile();
        if (e.code != ErrorCode::OK) {
            std::fprintf(stderr, "circuit_chain 编译失败: %s\n", e.message.c_str());
        } else {
            Result r;
            r.name = "circuit_chain";
            r.ticks = ticks;
            r.seconds = measure(*c, clk, ticks / 2, repeat);
            results.push_back(r);
        }
    }
    if (!explicit_files) {
        Net *clk = nullptr;
        auto c = makeDffBank(256, clk);
        auto e = c->compile();
        if (e.code != ErrorCode::OK) {
            std::fprintf(stderr, "circuit_dff 编译失败: %s\n", e.message.c_str());
        } else {
            Result r;
            r.name = "circuit_dff";
            r.ticks = ticks;
            r.seconds = measure(*c, clk, ticks / 2, repeat);
            results.push_back(r);
        }
    }

    std::printf("\n%-14s %13s %12s %10s %12s %12s\n", "场景", "完成周期", "编译(ms)", "耗时(s)", "M tick/秒", "M 周期/秒");
    std::printf("%s\n", std::string(82, '-').c_str());
    for (const auto &r : results) {
        const double mts = (double) r.ticks / r.seconds / 1e6;
        const double mcyc = r.done_cycles > 0 ? (double) r.done_cycles / r.seconds / 1e6 : 0.0;
        std::printf("%-14s %13lld %12.1f %10.3f %12.2f %12.2f\n", r.name.c_str(), r.done_cycles, r.compile_ms,
                    r.seconds, mts, mcyc);
    }
    std::printf("\n");
    return 0;
}
