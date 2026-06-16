// dcs_tool — 数字电路仿真 CLI 工具
//
// 用法:
//   dcs_tool --list-types              列出所有已注册的元件类型
//   dcs_tool --json-to-c <file.json>   将 JSON 电路描述转换为 C 代码
//   dcs_tool --help                    显示帮助
#include <cxxopts.hpp>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include "dcs/Circuit.h"
#include "dcs/ComponentFactory.h"

// 各元件类型的注册由静态库中全局对象在 main 前自动完成

namespace {

void listTypes() {
    auto &f = dsc::ComponentFactory::instance();
    const auto &metas = f.metas();

    std::cout << std::format("已注册元件类型: {} 种\n\n", metas.size());

    for (auto &[name, meta]: metas) {
        std::cout << std::format("  [{}]\n", meta.type_name);

        // 参数
        if (meta.params.empty()) {
            std::cout << "    参数: (无)\n";
        }
        else {
            std::cout << "    参数:\n";
            for (auto &p: meta.params) {
                std::cout << std::format("      {}  (默认: \"{}\")\n", p.name, p.default_val);
            }
        }

        // 输入引脚
        if (meta.input_pins.empty()) {
            std::cout << "    输入引脚: (由参数决定/动态)\n";
        }
        else {
            std::cout << "    输入引脚: ";
            for (size_t i = 0; i < meta.input_pins.size(); i++) {
                if (i > 0)
                    std::cout << ", ";
                std::cout << meta.input_pins[i];
            }
            std::cout << "\n";
        }

        // 输出引脚
        if (meta.output_pins.empty()) {
            std::cout << "    输出引脚: (由参数决定/动态)\n";
        }
        else {
            std::cout << "    输出引脚: ";
            for (size_t i = 0; i < meta.output_pins.size(); i++) {
                if (i > 0)
                    std::cout << ", ";
                std::cout << meta.output_pins[i];
            }
            std::cout << "\n";
        }

        std::cout << "\n";
    }
}

int jsonToC(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开文件 '{}'\n", path);
        return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string json_str = oss.str();

    std::string error;
    auto circuit = dsc::Circuit::fromJson(json_str, error);
    if (!circuit) {
        std::cerr << std::format("错误: {}\n", error);
        return 1;
    }

    // 编译（展开复合元件 + 环路检测），不执行 JIT
    try {
        circuit->compile();
    } catch (const std::exception &e) {
        std::cerr << std::format("编译错误: {}\n", e.what());
        return 1;
    }

    std::cout << circuit->generateC();
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    cxxopts::Options opts("dcs_tool", "数字电路仿真 CLI 工具");
    opts.add_options()("h,help", "显示帮助")("l,list-types", "列出所有已注册的元件类型及其参数")(
            "j,json-to-c", "将 JSON 电路描述编译为 C 代码", cxxopts::value<std::string>());

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << std::format("参数错误: {}\n", e.what());
        std::cerr << opts.help() << "\n";
        return 1;
    }

    if (result.count("help") || argc == 1) {
        std::cout << opts.help() << "\n";
        std::cout << R"(示例:
  dcs_tool --list-types
  dcs_tool --json-to-c circuit.json > circuit.c
)";
        return 0;
    }

    if (result.count("list-types")) {
        listTypes();
        return 0;
    }

    if (result.count("json-to-c")) {
        return jsonToC(result["json-to-c"].as<std::string>());
    }

    // 未匹配任何选项
    std::cout << opts.help() << "\n";
    return 1;
}
