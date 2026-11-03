// dcs — 数字电路仿真 CLI（合并 tool + simulator）
//
// 用法:
//   dcs circuit.json -t 10 -m       仿真运行
//   dcs --json-to-c circuit.json    输出 C 代码
//   dcs --list-types                列出元件类型
//   dcs --pins <type> [--param k=v] 查指定参数下元件的真实引脚
//   dcs --dll-info <path>           查看 DLL 元件引脚信息
//   dcs --help                      帮助
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cxxopts.hpp>
#include <dcs/Circuit.h>
#include <dcs/CircuitLibrary.h>
#include <dcs/ComponentFactory.h>
#include <dcs/Context.h>
#include <dcs/CircuitCheck.h>
#include <dcs/SchemaGen.h>
#include <dcs/util/MemDump.h>
#include <dcs/util/PathList.h>
#include <dcs/TruthTable.h>
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

#include "cli_common.h"

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
        std::cout << std::format("  [{}]  ({})\n", meta.type_name, f.categoryDisplayName(meta.category));
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
        j["category"] = meta.category;
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

// ---- --pins：按指定参数实例化元件，打印真实引脚 ----
// --list-types 里的引脚来自注册元数据，动态引脚（sel0..、in0..、out0..）只能标成
// "(动态)"，位宽也看不到。这里直接走工厂构造一次，输出的就是接线时能用的名字。
int printPins(const std::string &type, const std::vector<std::string> &raw_params, bool as_json) {
    std::unordered_map<std::string, std::string> params;
    for (auto &kv: raw_params) {
        auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::cerr << std::format("参数格式错误: \"{}\"，应为 key=value\n", kv);
            return 1;
        }
        params[kv.substr(0, eq)] = kv.substr(eq + 1);
    }

    auto &f = dsc::ComponentFactory::instance();
    std::string err;
    auto comp = f.create(type, "__pins__", params, err);
    if (!comp) {
        std::cerr << err << "\n";
        return 1;
    }

    auto mit = f.metas().find(type);
    const dsc::ComponentFactory::Meta *meta = mit == f.metas().end() ? nullptr : &mit->second;
    std::string category = meta ? f.categoryDisplayName(meta->category) : "";
    std::string description = meta ? meta->description : "";

    // 工厂对没声明过的参数是静默忽略的；查引脚时这样会把拼错的参数当成默认值，
    // 结果看起来"能用"实际是另一个元件形态，必须提醒
    if (meta) {
        for (auto &[k, v]: params) {
            bool known = std::any_of(meta->params.begin(), meta->params.end(),
                                     [&](const dsc::ComponentFactory::ParamMeta &pm) { return pm.name == k; });
            if (!known) std::cerr << std::format("警告: 类型 {} 没有参数 {}（已忽略）\n", type, k);
        }
    }

    // 参数按注册顺序显示；构造函数通过 setParam 记录的是实际生效的值，
    // 没记录的（有的元件参数不影响结构）退回默认值
    auto exported = comp->exportParams();
    std::vector<std::pair<std::string, std::string>> shown_params;
    if (meta) {
        for (auto &pm: meta->params) {
            auto it = exported.find(pm.name);
            shown_params.emplace_back(pm.name, it == exported.end() ? pm.default_val : it->second);
        }
    }
    else {
        for (auto &[k, v]: exported) shown_params.emplace_back(k, v);
    }

    if (as_json) {
        nlohmann::json j;
        j["type"] = type;
        j["category"] = category;
        j["description"] = description;
        j["params"] = nlohmann::json::object();
        for (auto &[k, v]: shown_params) j["params"][k] = v;
        auto pin_json = [](const auto &pins, bool output) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto &p: pins) {
                nlohmann::json pin{{"name", p->name()}, {"width", p->bit_width()}};
                if (output) {
                    pin["sequential"] = p->isSequential();
                    if (p->isTriState()) pin["tri_state"] = true;
                }
                arr.push_back(std::move(pin));
            }
            return arr;
        };
        j["inputs"] = pin_json(comp->inputs(), false);
        j["outputs"] = pin_json(comp->outputs(), true);
        std::cout << j.dump(2) << "\n";
        return 0;
    }

    std::cout << std::format("{}  ({})\n", type, category);
    if (!description.empty()) std::cout << std::format("  {}\n", description);
    if (!shown_params.empty()) {
        std::cout << "参数:\n";
        for (auto &[k, v]: shown_params) std::cout << std::format("  {} = {}\n", k, v);
    }
    // 引脚名按创建顺序排，位宽右对齐，方便一眼扫出哪根是总线
    auto print_pins = [](const char *title, const auto &pins, bool output) {
        std::cout << std::format("{}:\n", title);
        if (pins.empty()) {
            std::cout << "  (无)\n";
            return;
        }
        size_t w = 0;
        for (auto &p: pins) w = std::max<size_t>(w, p->name().size());
        for (auto &p: pins) {
            std::cout << std::format("  {:<{}}  {:>3}", p->name(), w, p->bit_width());
            if (output && p->isSequential()) std::cout << "  [sequential]";
            if (output && p->isTriState()) std::cout << "  [tri-state]";
            std::cout << "\n";
        }
    };
    print_pins("输入引脚", comp->inputs(), false);
    print_pins("输出引脚", comp->outputs(), true);
    return 0;
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
int jsonToC(const std::string &path, dsc::Context &ctx) {
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
        circuit = dsc::Circuit::fromJson(oss.str(), error, std::filesystem::path(path).parent_path().string(), &ctx);
    } catch (const std::exception &e) {
        std::cerr << std::format("错误: {}\n", e.what());
        return 1;
    }
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return 1;
    }
    // 必须真正编译一次：生成器拼错的 C 只有过编译器才能发现，只做网表校验会漏掉
    auto err = circuit->compile();
    if (err.code != dsc::ErrorCode::OK)
        std::cerr << std::format("// 编译错误: {}\n", err.message);
    std::cout << circuit->generateC();
    return err.code == dsc::ErrorCode::OK ? 0 : 1;
}

// ---- truth-table ----
int truthTable(const std::string &path, const std::string &inputs_csv, const std::string &output,
               dsc::Context &ctx) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开 '{}'\n", path);
        return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string error;
    auto circuit =
            dsc::Circuit::fromJson(oss.str(), error, std::filesystem::path(path).parent_path().string(), &ctx);
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return 1;
    }
    auto err = circuit->compile();
    if (err.code != dsc::ErrorCode::OK) {
        std::cerr << std::format("编译错误: {}\n", err.message);
        return 1;
    }

    std::vector<std::string> ins;
    std::stringstream ss(inputs_csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto b = item.find_first_not_of(" \t");
        const auto e = item.find_last_not_of(" \t");
        if (b != std::string::npos)
            ins.push_back(item.substr(b, e - b + 1));
    }

    dsc::TruthTable tt;
    if (!dsc::computeTruthTable(*circuit, ins, output, tt, error)) {
        std::cerr << std::format("错误: {}\n", error);
        return 1;
    }
    std::cout << dsc::formatTruthTable(tt);
    std::cout << "\n" << dsc::formatKarnaugh(tt);
    circuit->deinit();
    return 0;
}

// ---- simulate ----
int simulate(const std::string &path, uint64_t ticks, int delay_ms, bool monitor, bool forever,
              const std::string &vcd_path, const std::string &dump_mem, dsc::Context &ctx) {
    auto circuit = dcs_cli::loadCircuit(path, ctx);
    if (!circuit)
        return 1;

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

    // tick 计数用 64 位：跑一次完整内核启动动辄几十亿拍，32 位会在中途溢出变成负数。
    // 不需要逐拍观察（没有 VCD/逐拍打印/限速）时成块交给 JIT，
    // 省掉每拍一次 C++ 间接调用
    for (uint64_t t = 0; t < ticks;) {
        if (!vcd.is_open() && !monitor && delay_ms == 0) {
            const uint64_t chunk = (ticks - t > 65536) ? 65536 : (ticks - t);
            circuit->tickBatch(chunk);
            t += chunk;
            continue;
        }
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
        t++;
    }

    // 调试用：把存储器里的一段原样导出来。跑内核实测时，串口还没初始化
    // 之前内核的日志只在 printk 环形缓冲区里，只能靠它看到内核到底走到哪了
    if (!dump_mem.empty()) {
        auto *dm = dcs_cli::findFirstComponent<dsc::WishboneMemory>(*circuit);
        if (!dm) {
            std::cerr << "错误: 电路里没有 WishboneMemory，无法导出\n";
            return 1;
        }
        size_t addr, len;
        std::string perr;
        if (!dcs_cli::parseAddrLen(dump_mem, addr, len, perr)) {
            std::cerr << "错误: " << perr << "\n";
            return 1;
        }
        const size_t cap = dm->memSize();
        const uint8_t *p = dm->writableMemPtr();
        dsc::dumpMemoryHex(std::cout, p, cap, addr, len);
    }

    circuit->deinit();
    return 0;
}

// ---- 校验（--check）----
int checkFile(const std::string &path, dsc::Context &ctx) {
    std::string kind, error;
    if (!dsc::readJsonKind(path, kind, error)) {
        std::cerr << std::format("错误: {}\n", error);
        return 1;
    }
    const bool is_blueprint = kind == "blueprint";
    dsc::CheckReport report;
    const bool ok = is_blueprint ? dsc::checkBlueprintFile(path, &ctx, report, error)
                                 : dsc::checkCircuitFile(path, &ctx, report, error);
    if (!ok) {
        std::cerr << std::format("校验失败: {}\n", error);
        return 1;
    }
    std::cout << std::format("{}: {} 线网, {} 元件（展开后）\n", is_blueprint ? "蓝图" : "电路", report.nets,
                             report.components);
    if (!report.definitions.empty()) {
        std::cout << "蓝图定义:\n";
        for (auto &d: report.definitions)
            std::cout << "  " << d << "\n";
    }
    for (auto &w: report.warnings)
        std::cout << std::format("警告: {}\n", w);
    std::cout << "校验通过\n";
    return 0;
}

// ---- 列出蓝图定义（--list-definitions）----
int listDefinitions(const std::string &path, dsc::Context &ctx) {
    std::string error;
    if (!dsc::loadDefinitionsFromFile(path, &ctx, error)) {
        std::cerr << std::format("错误: {}\n", error);
        return 1;
    }
    std::vector<std::string> lines;
    for (auto &[name, def]: ctx.library().definitions())
        lines.push_back(def.source.empty() ? name : std::format("{}  <-  {}", name, def.source));
    std::sort(lines.begin(), lines.end());
    if (lines.empty()) {
        std::cout << "（没有蓝图定义）\n";
        return 0;
    }
    for (auto &l: lines)
        std::cout << l << "\n";
    return 0;
}

// ---- 元件 schema（--dump-schema）----
int dumpSchema() {
    std::cout << dsc::componentSchemaJson() << "\n";
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
            "json", "配合 --list-types / --pins：输出机器可读的 JSON 描述")("j,json-to-c", "JSON 电路转为 C 代码输出",
                                                                    cxxopts::value<std::string>())(
            "pins", "按参数实例化元件并打印真实引脚名和位宽", cxxopts::value<std::string>())(
            "param", "配合 --pins：参数 key=value（可重复）", cxxopts::value<std::vector<std::string>>())(
            "dll-info", "查看 DLL 元件引脚信息", cxxopts::value<std::string>())(
            "truth-table", "导出组合电路的真值表和卡诺图（需 --inputs 和 --output）")(
            "inputs", "真值表的输入网名，逗号分隔（第一个是最高位）", cxxopts::value<std::string>())(
            "output", "真值表的输出网名", cxxopts::value<std::string>())(
            "I,lib-path", "蓝图库查找路径（可重复）；definitions 里的相对路径先按引用文件所在目录找，再按这些目录顺序找",
            cxxopts::value<std::vector<std::string>>())(
            "check", "校验电路/蓝图 JSON：加载、展开、静态检查，不编译", cxxopts::value<std::string>())(
            "list-definitions", "列出 JSON 里的蓝图定义及来源", cxxopts::value<std::string>())(
            "dump-schema", "把元件类型 schema 输出到 stdout（重定向到文件即可）");

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
  dcs circuit.json --truth-table --inputs a,b --output y
                                    导出真值表和卡诺图
  dcs --list-types                  列出元件类型
  dcs --pins counter --param has_en=1
                                    查该参数下元件的真实引脚名和位宽
  dcs --dll-info adder8_dll.dll    查看 DLL 引脚信息
)";
        return 0;
    }

    // 蓝图库查找路径要在任何 fromJson 之前登记（电路 JSON 的 definitions 会用到）。
    // --lib-path 在前，环境变量 DCS_LIB_PATH（PATH 风格）在后
    dsc::Context ctx;
    std::vector<std::string> lib_paths;
    if (result.count("lib-path"))
        lib_paths = result["lib-path"].as<std::vector<std::string>>();
    dcs_cli::configureLibrary(ctx, lib_paths);

    if (result.count("dump-schema"))
        return dumpSchema();
    if (result.count("check"))
        return checkFile(result["check"].as<std::string>(), ctx);
    if (result.count("list-definitions"))
        return listDefinitions(result["list-definitions"].as<std::string>(), ctx);

    if (result.count("pins")) {
        std::vector<std::string> ps;
        if (result.count("param")) ps = result["param"].as<std::vector<std::string>>();
        return printPins(result["pins"].as<std::string>(), ps, result.count("json") != 0);
    }
    if (result.count("param")) {
        std::cerr << "--param 只能配合 --pins 使用\n";
        return 1;
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
        return jsonToC(result["json-to-c"].as<std::string>(), ctx);
    }
    if (result.count("truth-table")) {
        if (!result.count("circuit") || !result.count("inputs") || !result.count("output")) {
            std::cerr << "错误: --truth-table 需要 circuit.json、--inputs 和 --output\n";
            return 1;
        }
        return truthTable(result["circuit"].as<std::string>(), result["inputs"].as<std::string>(),
                          result["output"].as<std::string>(), ctx);
    }
    if (result.count("circuit")) {
        return simulate(result["circuit"].as<std::string>(), result["ticks"].as<uint64_t>(), result["delay"].as<int>(),
                        result.count("monitor") != 0, result.count("forever") != 0,
                        result.count("vcd") ? result["vcd"].as<std::string>() : std::string{},
                        result.count("dump-mem") ? result["dump-mem"].as<std::string>() : std::string{}, ctx);
    }

    std::cerr << "错误: 请指定 JSON 电路文件\n\n" << opts.help() << "\n";
    return 1;
}
