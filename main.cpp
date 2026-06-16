// digital_circuit_simulation — JSON 仿真运行器
//
// 用法:
//   digital_circuit_simulation -c circuit.json -t 20 -d 500
//   digital_circuit_simulation --circuit circuit.json --ticks 20 --delay 500
//
// 读取 JSON 电路描述，JIT 编译，仿真指定周期数，逐 tick 打印线网值。
// --delay 指定每个 tick 对应的现实时间（毫秒），默认 1000。
#include <dcs/Circuit.h>
#include <cxxopts.hpp>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

int main(int argc, char *argv[]) {
    cxxopts::Options opts("digital_circuit_simulation", "数字电路仿真运行器");
    opts.add_options()
        ("circuit",   "JSON 电路描述文件", cxxopts::value<std::string>())
        ("t,ticks",   "仿真 tick 数（默认 10）", cxxopts::value<int>()->default_value("10"))
        ("d,delay",   "每 tick 现实时间（毫秒，默认 1000）", cxxopts::value<int>()->default_value("1000"))
        ("m,monitor", "逐 tick 打印线网值")
        ("h,help",    "显示帮助")
        ;
    opts.parse_positional({"circuit"});
    opts.positional_help("circuit.json");

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << std::format("参数错误: {}\n", e.what());
        std::cerr << opts.help() << "\n";
        return 1;
    }

    if (result.count("help")) {
        std::cout << opts.help() << "\n";
        return 0;
    }
    if (!result.count("circuit")) {
        std::cerr << "错误: 请指定 JSON 电路文件\n\n";
        std::cerr << opts.help() << "\n";
        return 1;
    }

    std::string path = result["circuit"].as<std::string>();
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开 '{}'\n", path);
        return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();

    std::string error;
    auto circuit = dsc::Circuit::fromJson(oss.str(), error);
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return 1;
    }

    std::cout << std::format("电路: {} 线网, {} 元件\n",
                             circuit->netCount(), circuit->componentCount());

    try {
        circuit->compile();
    } catch (const std::exception &e) {
        std::cerr << std::format("编译失败: {}\n", e.what());
        return 1;
    }
    std::cout << "JIT 编译成功\n\n";

    circuit->init();

    int max_ticks = result["ticks"].as<int>();
    int delay_ms = result["delay"].as<int>();
    bool monitor = result.count("monitor") != 0;

    for (int t = 0; t < max_ticks; t++) {
        circuit->tick();
        if (monitor) {
            std::cout << std::format("=== Tick {} ===\n", t);
            for (int i = 0; i < circuit->netCount(); i++) {
                auto val = circuit->getWire(i);
                bool nonzero = false;
                for (auto b : val)
                    if (b) { nonzero = true; break; }
                if (nonzero) {
                    std::cout << std::format("  net {:2}: ", i);
                    for (int j = 0; j < 16; j++)
                        std::cout << std::format("{:02X}", val[j]);
                    std::cout << "\n";
                }
            }
            std::cout << std::flush;
        }
        if (t < max_ticks - 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    circuit->deinit();
    return 0;
}
