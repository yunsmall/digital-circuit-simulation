// dcs — 数字电路仿真 CLI（合并 tool + simulator）
//
// 用法:
//   dcs circuit.json -t 10 -m       仿真运行
//   dcs --json-to-c circuit.json    输出 C 代码
//   dcs --list-types                列出元件类型
//   dcs --dll-info <path>           查看 DLL 元件引脚信息
//   dcs --help                      帮助
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cxxopts.hpp>
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/components/memory/WishboneMemory.h>
#include <dcs/dll_interface.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

// --dll-info 要直接打开动态库读导出符号，只能用平台 API
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// ---- list-types ----
void listTypes() {
    auto &f = dsc::ComponentFactory::instance();
    const auto &metas = f.metas();
    std::cout << std::format("已注册元件类型: {} 种\n\n", metas.size());
    for (auto &[name, meta]: metas) {
        std::cout << std::format("  [{}]  ({})\n", meta.type_name, dsc::categoryName(meta.category));
        if (!meta.description.empty())
            std::cout << std::format("    {}\n", meta.description);
        if (meta.params.empty()) {
            std::cout << "    参数: (无)\n";
        }
        else {
            std::cout << "    参数:\n";
            for (auto &p: meta.params) {
                std::cout << std::format("      {}  (默认: \"{}\")", p.name, p.default_val);
                if (!p.valid_values.empty()) {
                    std::cout << "  可选: {";
                    for (size_t i = 0; i < p.valid_values.size(); i++)
                        std::cout << (i ? ", " : "") << p.valid_values[i];
                    std::cout << "}";
                }
                std::cout << "\n";
            }
        }
        if (meta.input_pins.empty()) {
            std::cout << "    输入引脚: (动态)\n";
        }
        else {
            std::cout << "    输入引脚: ";
            for (size_t i = 0; i < meta.input_pins.size(); i++)
                std::cout << (i ? ", " : "") << meta.input_pins[i];
            std::cout << "\n";
        }
        if (meta.output_pins.empty()) {
            std::cout << "    输出引脚: (动态)\n";
        }
        else {
            std::cout << "    输出引脚: ";
            for (size_t i = 0; i < meta.output_pins.size(); i++)
                std::cout << (i ? ", " : "") << meta.output_pins[i];
            std::cout << "\n";
        }
        std::cout << "\n";
    }
}

// ---- list-types（JSON：给外部工具生成编辑器/文档用）----
void listTypesJson() {
    auto &f = dsc::ComponentFactory::instance();
    nlohmann::json root;
    root["components"] = nlohmann::json::array();
    for (auto &[name, meta]: f.metas()) {
        nlohmann::json j;
        j["type"] = meta.type_name;
        j["category"] = dsc::categoryKey(meta.category);
        j["description"] = meta.description;
        j["params"] = nlohmann::json::array();
        for (auto &p: meta.params)
            j["params"].push_back({{"name", p.name}, {"default", p.default_val}, {"valid_values", p.valid_values}});
        j["inputs"] = meta.input_pins;
        j["outputs"] = meta.output_pins;
        root["components"].push_back(std::move(j));
    }
    std::cout << root.dump(2) << "\n";
}

// ---- dll-info ----
int dllInfo(const std::string &path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) {
        std::cerr << std::format("错误: 无法加载 DLL '{}'\n", path);
        return 1;
    }
    FARPROC fp = GetProcAddress(h, "dcs_dll_desc");
    if (!fp) {
        std::cerr << "DLL 缺少导出符号: dcs_dll_desc\n";
        FreeLibrary(h);
        return 1;
    }
    dcs_dll_descriptor_t *desc = nullptr;
    std::memcpy(&desc, &fp, sizeof(fp));
#else
    void *h = dlopen(path.c_str(), RTLD_NOW);
    if (!h) {
        std::cerr << std::format("错误: 无法加载 DLL '{}'\n", path);
        return 1;
    }
    void *sym = dlsym(h, "dcs_dll_desc");
    if (!sym) {
        std::cerr << "DLL 缺少导出符号: dcs_dll_desc\n";
        dlclose(h);
        return 1;
    }
    dcs_dll_descriptor_t *desc = reinterpret_cast<dcs_dll_descriptor_t *>(sym);
#endif

    std::cout << std::format("DLL 元件: {}\n", desc->type_name);
    std::cout << std::format("  输入引脚 ({} 个):\n", desc->num_inputs);
    for (int i = 0; i < desc->num_inputs; i++) {
        auto &p = desc->inputs[i];
        std::cout << std::format("    {}  ({} 位)\n", p.name, p.bit_width);
    }
    std::cout << std::format("  输出引脚 ({} 个):\n", desc->num_outputs);
    for (int i = 0; i < desc->num_outputs; i++) {
        auto &p = desc->outputs[i];
        const char *attr = p.is_sequential ? "时序" : "组合";
        const char *ts = p.is_tri_state ? " 三态" : "";
        std::cout << std::format("    {}  ({} 位, {}{})\n", p.name, p.bit_width, attr, ts);
    }

#ifdef _WIN32
    FreeLibrary(h);
#else
    dlclose(h);
#endif
    return 0;
}

// ---- json-to-c ----
int jsonToC(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开 '{}'\n", path);
        return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string error;
    std::unique_ptr<dsc::Circuit> circuit;
    try {
        circuit = dsc::Circuit::fromJson(oss.str(), error, std::filesystem::path(path).parent_path().string());
    } catch (const std::exception &e) {
        std::cerr << std::format("错误: {}\n", e.what());
        return 1;
    }
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return 1;
    }
    auto err = circuit->compile();
    if (err.code != dsc::ErrorCode::OK)
        std::cerr << std::format("// 编译错误: {}\n", err.message);
    std::cout << circuit->generateC();
    return err.code == dsc::ErrorCode::OK ? 0 : 1;
}

// ---- simulate ----
int simulate(const std::string &path, uint64_t ticks, int delay_ms, bool monitor, bool forever,
              const std::string &vcd_path, const std::string &dump_mem) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开 '{}'\n", path);
        return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string error;
    auto circuit = dsc::Circuit::fromJson(oss.str(), error, std::filesystem::path(path).parent_path().string());
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return 1;
    }
    std::cout << std::format("电路: {} 线网, {} 元件\n", circuit->netCount(), circuit->componentCount());

    auto err = circuit->compile();
    if (err.code != dsc::ErrorCode::OK) {
        std::cerr << std::format("编译错误: {}\n", err.message);
        return 1;
    }
    std::cout << "JIT 编译成功\n\n";
    circuit->init();

    // 可选：把线网变化导出成 VCD，便于用 GTKWave 观察时序
    std::ofstream vcd;
    std::vector<std::vector<uint8_t>> vcd_prev;
    if (!vcd_path.empty()) {
        vcd.open(vcd_path);
        if (!vcd) {
            std::cerr << std::format("错误: 无法写入 '{}'\n", vcd_path);
            return 1;
        }
        vcd << "$timescale 1ns $end\n$scope module dcs $end\n";
        for (int i = 0; i < circuit->netCount(); i++) {
            dsc::Net *n = circuit->nets()[i].get();
            int w = n->bit_width() > 0 ? n->bit_width() : 1;
            vcd << std::format("$var wire {} n{} {} $end\n", w, i, n->name());
        }
        vcd << "$upscope $end\n$enddefinitions $end\n";
        vcd_prev.assign(circuit->netCount(), std::vector<uint8_t>(16, 0));
    }

    // --forever：死循环在 JIT 里，C++ 不再逐拍往返，Ctrl+C 终止进程
    if (forever) {
        std::cout << "无限仿真中（Ctrl+C 停止）\n" << std::flush;
        circuit->tickForever();
    }

    // tick 计数用 64 位：跑一次完整内核启动动辄几十亿拍，32 位会在中途溢出变成负数
    for (uint64_t t = 0; t < ticks; t++) {
        circuit->tick();
        if (vcd.is_open()) {
            vcd << "#" << t << "\n";
            for (int i = 0; i < circuit->netCount(); i++) {
                auto val = circuit->getWire(i);
                if (val == vcd_prev[i])
                    continue;
                vcd_prev[i] = val;
                std::string bits;
                for (int b = 15; b >= 0; --b)
                    for (int k = 7; k >= 0; --k) {
                        int bit = (val[b] >> k) & 1;
                        if (!bits.empty() || bit)
                            bits.push_back(char('0' + bit));
                    }
                if (bits.empty())
                    bits = "0";
                vcd << "b" << bits << " n" << i << "\n";
            }
        }
        if (monitor) {
            std::cout << std::format("=== Tick {} ===\n", t);
            for (int i = 0; i < circuit->netCount(); i++) {
                auto val = circuit->getWire(i);
                bool nonzero = false;
                for (auto b: val)
                    if (b) {
                        nonzero = true;
                        break;
                    }
                if (nonzero) {
                    std::cout << std::format("  net {:2}: ", i);
                    for (int j = 0; j < 16; j++)
                        std::cout << std::format("{:02X}", val[j]);
                    std::cout << "\n";
                }
            }
            std::cout << std::flush;
        }
        if (delay_ms > 0 && t + 1 < ticks)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms)); // 0 也要睡的话就是每 tick 一次系统调用
    }

    // 调试用：把存储器里的一段原样导出来。跑内核实测时，串口还没初始化
    // 之前内核的日志只在 printk 环形缓冲区里，只能靠它看到内核到底走到哪了
    if (!dump_mem.empty()) {
        dsc::WishboneMemory *dm = nullptr;
        for (auto &c: circuit->components())
            if (auto *m = dynamic_cast<dsc::WishboneMemory *>(c.get())) {
                dm = m;
                break;
            }
        if (!dm) {
            std::cerr << "错误: 电路里没有 WishboneMemory，无法导出\n";
            return 1;
        }
        const std::string spec = dump_mem;
        const auto colon = spec.find(':');
        if (colon == std::string::npos) {
            std::cerr << "错误: --dump-mem 格式应为 addr:len（十六进制）\n";
            return 1;
        }
        size_t addr = std::stoull(spec.substr(0, colon), nullptr, 0);
        size_t len = std::stoull(spec.substr(colon + 1), nullptr, 0);
        const size_t cap = dm->memSize();
        const uint8_t *p = dm->writableMemPtr();
        for (size_t row = 0; row < len; row += 16) {
            const size_t off = (addr + row) & (cap - 1);
            std::cout << std::format("0x{:08X}  ", addr + row);
            for (size_t k = 0; k < 16; ++k) {
                if (row + k < len)
                    std::cout << std::format("{:02X} ", p[(off + k) & (cap - 1)]);
                else
                    std::cout << "   ";
            }
            std::cout << " ";
            for (size_t k = 0; k < 16 && row + k < len; ++k) {
                const char ch = (char) p[(off + k) & (cap - 1)];
                std::cout << (ch >= 0x20 && ch < 0x7F ? ch : '.');
            }
            std::cout << "\n";
        }
    }

    circuit->deinit();
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    cxxopts::Options opts("dcs", "数字电路仿真 CLI");

    // 仿真选项
    opts.add_options("仿真")("circuit", "JSON 电路描述文件（位置参数）", cxxopts::value<std::string>())(
            "t,ticks", "仿真 tick 数（默认 10）", cxxopts::value<uint64_t>()->default_value("10"))(
            "forever", "一直仿真直到 Ctrl+C（忽略 --ticks，循环留在 JIT 里）")(
            "d,delay", "每 tick 间隔毫秒（默认 0；逐拍观察时用 -m 配合较大的值）",
            cxxopts::value<int>()->default_value("0"))("m,monitor", "逐 tick 打印线网值")(
            "dump-mem", "仿真结束后按 addr:len（十六进制，物理地址）导出存储器内容",
            cxxopts::value<std::string>())(
            "vcd", "把线网变化导出为 VCD 波形（可用 GTKWave 打开）", cxxopts::value<std::string>());

    // 工具选项
    opts.add_options("工具")("l,list-types", "列出所有已注册的元件类型")(
            "json", "配合 --list-types：输出机器可读的 JSON 描述")("j,json-to-c", "JSON 电路转为 C 代码输出",
                                                                    cxxopts::value<std::string>())(
            "dll-info", "查看 DLL 元件引脚信息", cxxopts::value<std::string>());

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
        std::cout << "dcs 0.0.1\n";
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
  dcs circuit.json -t 20 -m         仿真 20 tick 并监控
  dcs --json-to-c circuit.json      输出等效 C 代码
  dcs --list-types                  列出元件类型
  dcs --dll-info adder8_dll.dll    查看 DLL 引脚信息
)";
        return 0;
    }

    if (result.count("list-types")) {
        if (result.count("json"))
            listTypesJson();
        else
            listTypes();
        return 0;
    }
    if (result.count("dll-info")) {
        return dllInfo(result["dll-info"].as<std::string>());
    }
    if (result.count("json-to-c")) {
        return jsonToC(result["json-to-c"].as<std::string>());
    }
    if (result.count("circuit")) {
        return simulate(result["circuit"].as<std::string>(), result["ticks"].as<uint64_t>(), result["delay"].as<int>(),
                        result.count("monitor") != 0, result.count("forever") != 0,
                        result.count("vcd") ? result["vcd"].as<std::string>() : std::string{},
                        result.count("dump-mem") ? result["dump-mem"].as<std::string>() : std::string{});
    }

    std::cerr << "错误: 请指定 JSON 电路文件\n\n" << opts.help() << "\n";
    return 1;
}
