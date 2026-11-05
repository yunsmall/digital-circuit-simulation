// dcs_soc — 整机（RISC-V SoC）仿真入口
//
// dcs 本体只做纯电路仿真；"把 ELF 灌进存储器、CPU 从入口开始跑、串口接宿主
// 键盘、按 tohost 判定结果、停下来的 PC 跟踪"这些是整机仿真特有的东西，
// 全部收在这一层，避免通用电路仿真 CLI 长出一堆 RISC-V 专用参数。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxopts.hpp>
#include <dcs/Circuit.h>
#include <dcs/CircuitLibrary.h>
#include <dcs/Context.h>
#include <dcs/util/MemDump.h>
#include <dcs/util/PathList.h>
#include <dcs/components/platform/RiscVCPU.h>
#include <dcs/components/platform/VirtioBlk.h>
#include <dcs/components/platform/WishboneDcache.h>
#include <dcs/components/io/UartFeed.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/util/ElfLoader.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "cli_common.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

// Windows 控制台的功能键由 _getch 返回 0x00/0xE0 前缀 + 扫描码两个字节，
// 而内核终端（busybox 的行编辑）认的是 ANSI/xterm 转义序列。不翻译的话
// 方向键、Home/End、Delete 在 shell 里全部没有反应
#ifdef _WIN32
const char *scanCodeToAnsi(int scan) {
    switch (scan) {
    case 72: return "\x1b[A";    // 上
    case 80: return "\x1b[B";    // 下
    case 77: return "\x1b[C";    // 右
    case 75: return "\x1b[D";    // 左
    case 71: return "\x1b[H";    // Home
    case 79: return "\x1b[F";    // End
    case 82: return "\x1b[2~";   // Insert
    case 83: return "\x1b[3~";   // Delete
    case 73: return "\x1b[5~";   // PageUp
    case 81: return "\x1b[6~";   // PageDown
    case 59: return "\x1bOP";    // F1
    case 60: return "\x1bOQ";    // F2
    case 61: return "\x1bOR";    // F3
    case 62: return "\x1bOS";    // F4
    case 63: return "\x1b[15~";  // F5
    case 64: return "\x1b[17~";  // F6
    case 65: return "\x1b[18~";  // F7
    case 66: return "\x1b[19~";  // F8
    case 67: return "\x1b[20~";  // F9
    case 68: return "\x1b[21~";  // F10
    case 133: return "\x1b[23~"; // F11
    case 134: return "\x1b[24~"; // F12
    default: return nullptr;
    }
}
#endif

// 从宿主读一个字符：终端走 _getch（阻塞、立即响应、不回显，回显交给内核终端驱动），
// 管道/重定向走阻塞 ReadFile。EOF 返回 -1
int readConsoleChar() {
#ifdef _WIN32
    // 一个功能键要展开成好几字节的转义序列，先把它发完再回头读键盘
    static char pending[8];
    static int pending_len = 0, pending_pos = 0;
    if (pending_pos < pending_len)
        return (unsigned char) pending[pending_pos++];
    if (_isatty(_fileno(stdin))) {
        int c = _getch();
        if (c == 0 || c == 0xE0) { // 功能键：后面跟一个扫描码
            const char *seq = scanCodeToAnsi(_getch());
            if (!seq)
                return -1; // 没有标准转义序列的扩展键（Ctrl+方向之类），丢弃
            pending_len = (int) std::strlen(seq);
            std::memcpy(pending, seq, pending_len);
            pending_pos = 1;
            return (unsigned char) pending[0];
        }
        return c & 0xFF;
    }
    char c = 0;
    DWORD n = 0;
    if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &n, nullptr) && n == 1)
        return (unsigned char) c;
    return -1;
#else
    unsigned char c = 0;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    return n == 1 ? (int) c : -1;
#endif
}

dsc::WishboneMemory *firstMemory(dsc::Circuit &circuit) {
    return dcs_cli::findFirstComponent<dsc::WishboneMemory>(circuit);
}

// 把整个映像按存储器的地址别名语义灌进去：镜像可以链接在 DRAM_BASE(0x80000000)
// 这类高地址，而存储器只有低 addr_width 根地址线，高位在硬件上就是别名回来
bool loadImage(dsc::WishboneMemory *mem, const dsc::ElfImage &img) {
    const size_t cap = mem->memSize();
    size_t load_off = static_cast<size_t>(img.base) & (cap - 1);
    const uint8_t *src = img.data.data();
    size_t remain = img.data.size();
    while (remain > 0) {
        const size_t chunk = (std::min)(remain, cap - load_off);
        std::memcpy(mem->writableMemPtr() + load_off, src, chunk);
        src += chunk;
        remain -= chunk;
        load_off = (load_off + chunk) & (cap - 1);
    }
    return true;
}

int runSoc(const std::string &path, const std::string &elf_path, const std::string &disk_path, uint64_t ticks,
           int delay_ms, bool forever, const std::string &dump_mem, const std::string &feed_file,
           uint64_t feed_after, const std::string &watch_file, bool trace_cpu, dsc::Context &ctx) {
    // --forever 不会正常返回（tickForever 或 Ctrl+C 结束进程），后段的 dump-mem
    // 永远执行不到；早点报错，不要让参数静默失效
    if (forever && !dump_mem.empty()) {
        std::cerr << "错误: --forever 下仿真不会正常结束，--dump-mem 无法执行\n";
        return 1;
    }
    auto circuit = dcs_cli::loadCircuit(path, ctx);
    if (!circuit)
        return 1;

    // 运行期换盘：--disk 覆盖 JSON 里的 file 后端路径。必须在 compile() 之前，
    // 容量和只读特性是生成 config 空间时按介质算的
    if (!disk_path.empty()) {
        int applied = 0;
        for (auto &comp: circuit->components()) {
            if (comp->typeName() != "virtio_blk")
                continue;
            auto *blk = dynamic_cast<dsc::VirtioBlk *>(comp.get());
            if (!blk)
                continue;
            std::string derr;
            if (!blk->setImagePath(circuit->context(), disk_path, derr)) {
                std::cerr << derr << "\n";
                return 1;
            }
            applied++;
        }
        if (applied == 0)
            std::cerr << "警告: 电路里没有 virtio_blk，--disk 未生效\n";
    }
    // 整机跑 Linux 需要根文件系统；没给介质时早点说清楚，别等内核挂载失败
    for (auto &comp: circuit->components()) {
        if (comp->typeName() != "virtio_blk")
            continue;
        auto *blk = dynamic_cast<dsc::VirtioBlk *>(comp.get());
        if (blk && blk->backendName() == "none") {
            std::cerr << "错误: virtio_blk 没有介质，用 --disk 指定磁盘镜像\n";
            return 1;
        }
    }

    auto err = circuit->compile();
    if (err.code != dsc::ErrorCode::OK) {
        std::cerr << std::format("编译错误: {}\n", err.message);
        return 1;
    }
    std::cout << "JIT 编译成功\n\n";
    circuit->init();

    dsc::WishboneMemory *mem = firstMemory(*circuit);
    if (!mem) {
        std::cerr << "错误: 电路里没有 WishboneMemory，无法加载 ELF\n";
        return 1;
    }
    auto img = dsc::loadElf(elf_path);
    if (!img.ok) {
        std::cerr << std::format("ELF 错误: {}\n", img.error);
        return 1;
    }
    loadImage(mem, img);
    // 链接在高地址的镜像必须从自己的入口开始执行，否则 PC 相对寻址全按 0 基址算。
    // 位宽必须对上：拿 32 位核跑 RV64 代码只会得到一串非法指令，早点报出来
    bool cpu_matched = false;
    dsc::RiscVCPU<32> *cpu32 = nullptr;
    dsc::RiscVCPU<64> *cpu64 = nullptr;
    for (auto &c: circuit->components()) {
        if (auto *c32 = dynamic_cast<dsc::RiscVCPU<32> *>(c.get())) {
            cpu32 = c32;
            if (!img.is64) {
                c32->setPc((uint32_t) img.entry);
                cpu_matched = true;
            }
        } else if (auto *c64 = dynamic_cast<dsc::RiscVCPU<64> *>(c.get())) {
            cpu64 = c64;
            if (img.is64) {
                c64->setPc(img.entry);
                cpu_matched = true;
            }
        }
    }
    if (!cpu_matched) {
        std::cerr << std::format("错误: ELF 是 {}-bit，但电路里没有对应位宽的 riscv_cpu（JSON 里用 \"xlen\": \"{}\"）\n",
                                 img.is64 ? 64 : 32, img.is64 ? 64 : 32);
        return 1;
    }
    const size_t cap = mem->memSize();
    const uint32_t tohost_addr = static_cast<uint32_t>(img.tohost & (cap - 1));
    // tohost 的 4 字节必须整块落在存储器里；符号地址可以落在末尾 1..3 字节，
    // 直接 memcpy 会读越界
    const bool tohost_ok = img.tohost != 0 && (size_t) tohost_addr + 4 <= cap;
    if (img.tohost != 0 && !tohost_ok)
        std::cerr << std::format("警告: tohost 地址 0x{:X} 不在存储器范围（容量 0x{:X}），"
                                 "无法按 tohost 判定结果\n", img.tohost, cap);
    std::cout << std::format("ELF: {} 字节 @ 0x{:X}，入口 0x{:X}，tohost 0x{:X}\n", img.data.size(), img.base,
                             img.entry, img.tohost);

    // UartFeed 只提供队列；读 stdin 的活在这里单独开线程，仿真热路径上没有任何系统调用
    dsc::UartFeed *uart_feed = nullptr;
    for (auto &c: circuit->components())
        if (auto *u = dynamic_cast<dsc::UartFeed *>(c.get())) {
            uart_feed = u;
            break;
        }
    // 只在 --forever 时起读线程：有限 tick 模式下会返回、circuit 被析构，
    // detach 的线程再去 push 就是访问已释放对象
    if (forever && uart_feed) {
        std::thread([uart_feed] {
            for (;;) {
                int ch = readConsoleChar();
                if (ch < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                while (!uart_feed->push((uint8_t) ch))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }

    // --feed：把一段脚本预先塞进串口输入队列，启动完成后自动执行。
    // 每次改动机器模型都要重启内核跑几十亿拍，逐个命令交互验证太慢，
    // 用这个入口一次跑完一整组验证
    std::string feed_data;
    size_t feed_pos = 0;
    if (!feed_file.empty()) {
        std::ifstream ff(feed_file, std::ios::binary);
        if (!ff) {
            std::cerr << std::format("错误: 无法打开喂给串口的文件 '{}'\n", feed_file);
            return 1;
        }
        std::ostringstream fb;
        fb << ff.rdbuf();
        feed_data = fb.str();
        if (forever)
            std::cout << "注意: --feed 只在有限 tick 模式下生效，--forever 请手动交互\n";
    }

    if (forever) {
        if (watch_file.empty()) {
            std::cout << "无限仿真中（Ctrl+C 停止）\n" << std::flush;
            circuit->tickForever();
            // tickForever 只在运行期错误时返回；不查错误会继续走到 --watch 分支并报出误导性的参数错误
            if (circuit->hasError()) {
                std::cerr << std::format("运行错误: {}\n", circuit->tickError().message);
                return 1;
            }
        }
        // --watch：仿真不退出，外部往文件里追加内容就是往串口发命令。
        // 每批 tick 结束后检查文件增量，这样改硬件模型后跑一次就能反复验证，
        // 不必每次重启内核
        if (!uart_feed) {
            std::cerr << "错误: --watch 需要电路里有 uart_feed 元件\n";
            return 1;
        }
        std::cout << std::format("无限仿真中，往 {} 追加内容即可向串口发送命令（Ctrl+C 停止）\n",
                                 watch_file) << std::flush;
        std::ifstream wf_probe(watch_file, std::ios::binary);
        wf_probe.seekg(0, std::ios::end);
        uint64_t watch_pos = wf_probe ? (uint64_t) wf_probe.tellg() : 0;
        // 队列满时不能忙等：UartFeed 的消费者就是 tick 本身，阻塞在这里就没人推进仿真
        std::string pending;
        size_t pending_pos = 0;
        // --trace-cpu：连续多批 PC 不变就报一次现场，用来区分"真死循环"和"wfi 等中断"
        uint64_t trace_last_pc = ~0ull;
        int trace_stuck = 0;
        int trace_reports = 0;
        for (;;) {
            circuit->tickBatch(200000);
            // 总线冲突时批量入口会提前返回，不查就在这里无限空转
            if (circuit->hasError()) {
                std::cerr << std::format("运行错误: {}\n", circuit->tickError().message);
                return 1;
            }
            if (trace_cpu) {
                const uint64_t pc = cpu64 ? (uint64_t) cpu64->pc() : (cpu32 ? (uint64_t) cpu32->pc() : 0);
                const bool halted = cpu64 ? cpu64->halted() : (cpu32 ? cpu32->halted() : false);
                if (pc == trace_last_pc) {
                    // 内核 idle 会被周期定时器短暂唤醒，PC 变了又回来，所以只报前几次
                    if (++trace_stuck == 20 && trace_reports < 5) {
                        trace_reports++;
                        const uint64_t ra = cpu64 ? (uint64_t) cpu64->reg(1) : (cpu32 ? (uint64_t) cpu32->reg(1) : 0);
                        const uint64_t sp = cpu64 ? (uint64_t) cpu64->reg(2) : (cpu32 ? (uint64_t) cpu32->reg(2) : 0);
                        const uint64_t a0 = cpu64 ? (uint64_t) cpu64->reg(10) : (cpu32 ? (uint64_t) cpu32->reg(10) : 0);
                        std::fprintf(stderr, "[dcs] cpu 停在 PC=0x%llX halted=%d ra=0x%llX sp=0x%llX a0=0x%llX\n",
                                     (unsigned long long) pc, halted ? 1 : 0, (unsigned long long) ra,
                                     (unsigned long long) sp, (unsigned long long) a0);
                        std::fflush(stderr);
                    }
                } else {
                    trace_stuck = 0;
                    trace_last_pc = pc;
                }
            }
            std::error_code ec;
            const auto sz = std::filesystem::file_size(watch_file, ec);
            if (!ec && sz > watch_pos) {
                std::ifstream wf(watch_file, std::ios::binary);
                if (wf) {
                    wf.seekg((std::streamoff) watch_pos);
                    std::string chunk((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
                    watch_pos = sz;
                    if (pending_pos) {
                        pending.erase(0, pending_pos);
                        pending_pos = 0;
                    }
                    pending += chunk;
                }
            }
            while (pending_pos < pending.size() && uart_feed->push((uint8_t) pending[pending_pos]))
                pending_pos++;
            if (pending_pos > 65536) {
                pending.erase(0, pending_pos);
                pending_pos = 0;
            }
        }
    }

    // tick 计数用 64 位：跑一次完整内核启动动辄几十亿拍，32 位会在中途溢出。
    // 按块把循环留在 JIT 里：每拍一次 C++ 间接调用 + tohost 检查对整机仿真来说
    // 都是实打实的开销；块内没有需要逐拍观察的东西（--delay 逐拍观察时才退回单拍）。
    // 喂数据的边界要精确落在 feed_after 上，所以块不跨过那个点
    constexpr uint64_t kBatch = 65536;
    for (uint64_t t = 0; t < ticks;) {
        uint64_t chunk = ticks - t;
        if (delay_ms > 0) {
            circuit->tick();
            chunk = 1;
        } else {
            if (chunk > kBatch)
                chunk = kBatch;
            // 队列满就停在这里，等下一拍 UartFeed 发走一个字节再继续，不能在仿真线程里忙等
            if (feed_pos < feed_data.size() && t < feed_after && t + chunk > feed_after)
                chunk = feed_after - t;
            circuit->tickBatch(chunk);
        }
        if (circuit->hasError()) {
            std::cerr << std::format("运行错误: {}\n", circuit->tickError().message);
            return 1;
        }
        t += chunk;
        if (feed_pos < feed_data.size() && t >= feed_after) {
            while (feed_pos < feed_data.size() && uart_feed->push((uint8_t) feed_data[feed_pos]))
                feed_pos++;
        }
        // 用 ELF 里的 tohost 符号判断有没有（可能是 0），不能用掩码后的地址：
        // 符号地址低位全 0 时 tohost_addr 也是 0，会被误判成"没有 tohost"
        if (tohost_ok) { // riscv-tests 约定：写 1 通过，写 (用例号<<1)|1 失败
            uint32_t r = 0;
            std::memcpy(&r, mem->writableMemPtr() + tohost_addr, 4);
            if (r) {
                std::cout << std::format("\n程序结束: {} (tohost={})\n", r == 1 ? "通过" : "失败", r);
                return r == 1 ? 0 : 1;
            }
        }
        if (delay_ms > 0 && t + 1 < ticks)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    // 有数据缓存时按需打印命中率：调缓存参数（行数）时靠它判断值不值得加
    if (std::getenv("DCS_DCACHE_STATS")) {
        for (auto &c: circuit->components())
            if (auto *dc = dynamic_cast<dsc::WishboneDcache *>(c.get())) {
                const uint64_t acc = dc->accesses();
                std::fprintf(stderr, "[dcs] dcache: %llu 次访问，命中 %.1f%%（%llu/%llu）\n",
                             (unsigned long long) acc,
                             acc ? 100.0 * (double) dc->hits() / (double) acc : 0.0,
                             (unsigned long long) dc->hits(), (unsigned long long) dc->misses());
            }
    }

    // 仿真结束后打印 CPU 的 PC。总线地址只在取指缺失/访存时更新，
    // 小循环全命中 icache 时 wb_adr 会一直停在旧值，光看波形分不清是在跑还是卡死
    for (auto &c: circuit->components()) {
        if (auto *c32 = dynamic_cast<dsc::RiscVCPU<32> *>(c.get()))
            std::cout << std::format("CPU32 PC=0x{:X} priv={}\n", (uint32_t) c32->pc(), (int) c32->priv());
        else if (auto *c64 = dynamic_cast<dsc::RiscVCPU<64> *>(c.get())) {
            std::cout << std::format("CPU64 PC=0x{:X} priv={}\n", (uint64_t) c64->pc(), (int) c64->priv());
            auto trace = c64->pcTrace();
            std::cout << "PC trace:";
            for (auto p: trace)
                std::cout << std::format(" {:08X}", p);
            std::cout << "\n";
        }
    }

    // 把存储器里的一段原样导出来。串口还没初始化之前内核的日志只在 printk
    // 环形缓冲区里，只能靠它看到内核到底走到哪了
    if (!dump_mem.empty()) {
        size_t addr, len;
        std::string perr;
        if (!dcs_cli::parseAddrLen(dump_mem, addr, len, perr)) {
            std::cerr << "错误: " << perr << "\n";
            return 1;
        }
        const uint8_t *p = mem->writableMemPtr();
        dsc::dumpMemoryHex(std::cout, p, cap, addr, len);
    }

    circuit->deinit();
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    cxxopts::Options opts("dcs_soc", "整机仿真 CLI：把 RISC-V ELF 灌进电路里的存储器并运行");

    opts.add_options("仿真")("circuit", "JSON 电路描述文件（位置参数）", cxxopts::value<std::string>())(
            "elf", "要加载的 RISC-V ELF（镜像、入口地址、tohost 都从它读）", cxxopts::value<std::string>())(
            "disk", "virtio_blk 的后备镜像，覆盖 JSON 里的 backend_opts.path", cxxopts::value<std::string>())(
            "t,ticks", "仿真 tick 数（默认 10）", cxxopts::value<uint64_t>()->default_value("10"))(
            "forever", "一直仿真直到 Ctrl+C（忽略 --ticks，循环留在 JIT 里）")(
            "d,delay", "每 tick 间隔毫秒（默认 0）", cxxopts::value<int>()->default_value("0"))(
            "feed", "启动后自动喂给串口的脚本文件（一行一条命令）", cxxopts::value<std::string>())(
            "feed-after", "到第几拍才开始喂脚本（默认 0；内核启动到 shell 约 35 亿拍）",
            cxxopts::value<uint64_t>()->default_value("0"))(
            "watch", "仿真运行期间监视该文件，追加的内容立刻送进串口（配合 --forever，不必重启仿真）",
            cxxopts::value<std::string>())(
            "dump-mem", "仿真结束后按 addr:len（十六进制，物理地址）导出存储器内容",
            cxxopts::value<std::string>())(
            "trace-cpu", "长时间 PC 不变时打印 CPU 现场（调试死循环 / 等中断用）")(
            "I,lib-path", "蓝图库查找路径（可重复）；definitions 里的相对路径先按引用文件所在目录找，再按这些目录顺序找",
            cxxopts::value<std::vector<std::string>>());

    opts.add_options("其他")("h,help", "显示帮助")("v,version", "显示版本号");

    opts.parse_positional({"circuit"});
    opts.positional_help("circuit.json");

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << std::format("参数错误: {}\n{}\n", e.what(), opts.help());
        return 1;
    }

    if (result.count("version")) {
        std::cout << "dcs_soc 0.0.1\n";
        return 0;
    }
    if (result.count("help") || argc == 1) {
        std::cout << opts.help() << "\n";
#ifdef DCS_USE_TCC
        std::cout << "JIT 后端: TinyCC\n\n";
#else
        std::cout << "JIT 后端: LLVM/Clang\n\n";
#endif
        std::cout << R"(示例:
  dcs_soc circuit.json --elf boot.elf --forever        串口交互跑 Linux
  dcs_soc circuit.json --elf boot.elf --disk disk.img --forever
                                                       运行期换一块磁盘镜像
  dcs_soc circuit.json --elf boot.elf --forever --watch /tmp/cmds.txt
                                                       仿真不退出，往文件追加即发命令
  dcs_soc circuit.json --elf test.elf -t 1000000       跑 riscv-tests，按 tohost 判定
  dcs_soc circuit.json --elf boot.elf -t 4000000000 --feed cmds.txt --feed-after 3500000000
                                                       启动后自动执行一串命令
)";
        return 0;
    }

    // 蓝图库查找路径要在 fromJson 之前登记（电路 JSON 的 definitions 会用到）。
    // --lib-path 在前，环境变量 DCS_LIB_PATH（PATH 风格）在后
    dsc::Context ctx;
    std::vector<std::string> lib_paths;
    if (result.count("lib-path"))
        lib_paths = result["lib-path"].as<std::vector<std::string>>();
    dcs_cli::configureLibrary(ctx, lib_paths);

    if (!result.count("circuit")) {
        std::cerr << "错误: 请指定 JSON 电路文件\n\n" << opts.help() << "\n";
        return 1;
    }
    if (!result.count("elf")) {
        std::cerr << "错误: 整机仿真必须用 --elf 指定要运行的 RISC-V ELF\n";
        return 1;
    }
    return runSoc(result["circuit"].as<std::string>(), result["elf"].as<std::string>(),
                  result.count("disk") ? result["disk"].as<std::string>() : std::string{},
                  result["ticks"].as<uint64_t>(), result["delay"].as<int>(), result.count("forever") != 0,
                  result.count("dump-mem") ? result["dump-mem"].as<std::string>() : std::string{},
                  result.count("feed") ? result["feed"].as<std::string>() : std::string{},
                  result["feed-after"].as<uint64_t>(),
                  result.count("watch") ? result["watch"].as<std::string>() : std::string{},
                  result.count("trace-cpu") != 0, ctx);
}
