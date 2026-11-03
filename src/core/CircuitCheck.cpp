// 电路/蓝图文件的校验工具实现
#include "dcs/CircuitCheck.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/Context.h"

namespace dsc {

namespace {

namespace fs = std::filesystem;

bool readFile(const std::string &path, std::string &out, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("无法打开文件: {}", path);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool parseJson(const std::string &text, const std::string &path, nlohmann::json &out, std::string &error) {
    try {
        out = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error &e) {
        error = std::format("{} 解析失败: {}", path, e.what());
        return false;
    }
    return true;
}

std::string canonString(const fs::path &p) {
    std::error_code ec;
    return fs::weakly_canonical(p, ec).string();
}

CircuitLibrary &libraryOf(Context *ctx) {
    return ctx ? ctx->library() : Context::global().library();
}

std::vector<std::string> describeDefinitions(const CircuitLibrary &lib) {
    std::vector<std::string> out;
    for (auto &[name, def]: lib.definitions())
        out.push_back(def.source.empty() ? name : std::format("{} ({})", name, def.source));
    std::sort(out.begin(), out.end());
    return out;
}

// 单定义蓝图本身没有名字，导入时要从引用处的键取名。独立校验/列出时没有引用处，
// 用文件名做键；库里已有同名定义就加后缀。注意定义里写了 $meta.name 时以它为准
// （那是文件自己声明的规范名字，与普通导入一样会覆盖同名定义）
std::string keyForUnnamedBlueprint(const CircuitLibrary &lib, const fs::path &p) {
    std::string base = p.stem().string();
    if (base.empty())
        base = "blueprint";
    std::string key = base;
    for (int i = 1; lib.exists(key); i++)
        key = std::format("{}_{}", base, i);
    return key;
}

} // namespace

bool readJsonKind(const std::string &path, std::string &kind, std::string &error) {
    std::string text;
    if (!readFile(path, text, error))
        return false;
    nlohmann::json root;
    if (!parseJson(text, path, root, error))
        return false;
    kind = "circuit";
    if (!root.is_object()) {
        error = std::format("{} 的根必须是对象", path);
        return false;
    }
    if (auto m = root.find("$meta"); m != root.end() && m->is_object()) {
        if (auto k = m->find("kind"); k != m->end()) {
            if (!k->is_string()) {
                error = std::format("{}: $meta.kind 必须是字符串", path);
                return false;
            }
            kind = k->get<std::string>();
        }
    }
    return true;
}

bool loadDefinitionsFromFile(const std::string &path, Context *ctx, std::string &error) {
    std::string kind;
    if (!readJsonKind(path, kind, error))
        return false;
    const fs::path p(path);
    const fs::path base = p.parent_path().empty() ? fs::path(".") : p.parent_path();
    if (kind == "blueprint") {
        // 用只含一个引用的最小电路把文件里的定义导进上下文
        nlohmann::json wrapper;
        wrapper["definitions"] = nlohmann::json::object();
        wrapper["definitions"][keyForUnnamedBlueprint(libraryOf(ctx), p)] = p.filename().string();
        wrapper["components"] = nlohmann::json::array();
        std::string err;
        auto c = Circuit::fromJson(wrapper.dump(), err, base.string(), ctx);
        if (!c) {
            error = err;
            return false;
        }
        return true;
    }
    std::string text;
    if (!readFile(path, text, error))
        return false;
    nlohmann::json root;
    if (!parseJson(text, path, root, error))
        return false;
    if (!root.is_object()) {
        error = std::format("{} 的根必须是对象", path);
        return false;
    }
    // 只导入 definitions：电路文件里可能没有 components，元件也可能因为外部
    // 文件（ROM/DLL）缺失而建不出来——列定义/读蓝图不该被这些副作用拖累
    nlohmann::json wrapper;
    wrapper["components"] = nlohmann::json::array();
    if (root.contains("definitions"))
        wrapper["definitions"] = root["definitions"];
    std::string err;
    auto c = Circuit::fromJson(wrapper.dump(), err, base.string(), ctx);
    if (!c) {
        error = err;
        return false;
    }
    return true;
}

bool checkCircuitFile(const std::string &path, Context *ctx, CheckReport &report, std::string &error) {
    std::string text;
    if (!readFile(path, text, error))
        return false;
    const fs::path p(path);
    const fs::path base = p.parent_path().empty() ? fs::path(".") : p.parent_path();
    std::string err;
    auto circuit = Circuit::fromJson(text, err, base.string(), ctx);
    if (!circuit) {
        error = err;
        return false;
    }
    auto e = circuit->validate(&report.warnings);
    if (e.code != ErrorCode::OK) {
        error = e.message;
        return false;
    }
    report.nets = circuit->netCount();
    report.components = circuit->componentCount();
    report.definitions = describeDefinitions(libraryOf(ctx));
    return true;
}

bool checkBlueprintFile(const std::string &path, Context *ctx, CheckReport &report, std::string &error) {
    CircuitLibrary &lib = libraryOf(ctx);
    // 记录本次导入覆盖了哪些定义：只被引用的文件（自己不定义任何东西）也要校验依赖。
    // 不能只看"名字是否新出现"——依赖蓝图可能已经在库里，被重新导入后名字不变，
    // 只有库分配的导入编号能反映它这次被读过
    std::unordered_map<std::string, std::size_t> before;
    for (auto &[name, def]: lib.definitions())
        before[name] = def.import_serial;
    if (!loadDefinitionsFromFile(path, ctx, error))
        return false;

    // 本文件引入的定义：这次新建或覆盖的，加上 source 指向本文件的（重复校验无害）
    const std::string self = canonString(path);
    std::vector<std::string> names;
    for (auto &[name, def]: lib.definitions()) {
        auto it = before.find(name);
        const bool imported_now = it == before.end() || it->second != def.import_serial;
        const bool from_self = !def.source.empty() && canonString(def.source) == self;
        if (imported_now || from_self)
            names.push_back(name);
    }
    if (names.empty()) {
        error = std::format("{} 里没有可校验的蓝图定义", path);
        return false;
    }
    std::sort(names.begin(), names.end());

    const fs::path p(path);
    const fs::path base = p.parent_path().empty() ? fs::path(".") : p.parent_path();
    for (auto &name: names) {
        const auto *def = lib.find(name);
        if (!def)
            continue;
        nlohmann::json j;
        nlohmann::json nets = nlohmann::json::array();
        nlohmann::json conns = nlohmann::json::object();
        for (auto &[pin, net]: def->input_expose) {
            const std::string n = "__in_" + pin;
            nets.push_back(n);
            conns[pin] = n;
        }
        for (auto &[pin, net]: def->output_expose) {
            const std::string n = "__out_" + pin;
            nets.push_back(n);
            conns[pin] = n;
        }
        nlohmann::json comp;
        comp["type"] = "composite";
        comp["name"] = "__dcs_top__";
        comp["params"] = {{"definition", name}};
        comp["connections"] = conns;
        j["nets"] = nets;
        j["components"] = nlohmann::json::array({comp});

        std::string err;
        auto circuit = Circuit::fromJson(j.dump(), err, base.string(), ctx);
        if (!circuit) {
            error = std::format("蓝图 '{}' 实例化失败: {}", name, err);
            return false;
        }
        std::vector<std::string> warnings;
        auto e = circuit->validate(&warnings);
        if (e.code != ErrorCode::OK) {
            error = std::format("蓝图 '{}' 检查失败: {}", name, e.message);
            return false;
        }
        report.nets += circuit->netCount();
        report.components += circuit->componentCount();
        report.warnings.insert(report.warnings.end(), warnings.begin(), warnings.end());
    }
    report.definitions = describeDefinitions(lib);
    return true;
}

} // namespace dsc
