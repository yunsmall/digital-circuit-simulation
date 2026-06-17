// dcs — 数字电路仿真 CLI（合并 tool + simulator）
//
// 用法:
//   dcs circuit.json -t 10 -m       仿真运行
//   dcs --json-to-c circuit.json    输出 C 代码
//   dcs --list-types                列出元件类型
//   dcs --dll-info <path>           查看 DLL 元件引脚信息
//   dcs --help                      帮助
#include <chrono>
#include <cstdio>
#include <cxxopts.hpp>
#include <dcs/Circuit.h>
#include <dcs/ComponentFactory.h>
#include <dcs/dll_interface.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

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
        std::cout << std::format("  [{}]\n", meta.type_name);
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
int simulate(const std::string &path, int ticks, int delay_ms, bool monitor) {
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

    for (int t = 0; t < ticks; t++) {
        circuit->tick();
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
        if (t < ticks - 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    circuit->deinit();
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    cxxopts::Options opts("dcs", "数字电路仿真 CLI");

    // 仿真选项
    opts.add_options("仿真")("circuit", "JSON 电路描述文件（位置参数）", cxxopts::value<std::string>())(
            "t,ticks", "仿真 tick 数（默认 10）", cxxopts::value<int>()->default_value("10"))(
            "d,delay", "每 tick 间隔毫秒（默认 1000）",
            cxxopts::value<int>()->default_value("1000"))("m,monitor", "逐 tick 打印线网值");

    // 工具选项
    opts.add_options("工具")("l,list-types", "列出所有已注册的元件类型")("j,json-to-c", "JSON 电路转为 C 代码输出",
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
        return simulate(result["circuit"].as<std::string>(), result["ticks"].as<int>(), result["delay"].as<int>(),
                        result.count("monitor") != 0);
    }

    std::cerr << "错误: 请指定 JSON 电路文件\n\n" << opts.help() << "\n";
    return 1;
}
