// 电路/蓝图文件的校验工具实现
#include "dcs/CircuitCheck.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

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
    if (auto m = root.find("$meta"); m != root.end() && m->is_object())
        kind = m->value("kind", std::string{"circuit"});
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
        wrapper["definitions"]["__dcs_check__"] = p.filename().string();
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
    std::string err;
    auto c = Circuit::fromJson(text, err, base.string(), ctx);
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
    // 记录本次导入新增的定义：只被引用的文件（自己不定义任何东西）也要校验它的依赖
    std::vector<std::string> before;
    for (auto &[name, def]: lib.definitions())
        before.push_back(name);
    if (!loadDefinitionsFromFile(path, ctx, error))
        return false;

    // 本文件引入的定义：新登记的名字，加上 source 指向本文件的（重复校验无害）
    const std::string self = canonString(path);
    std::vector<std::string> names;
    for (auto &[name, def]: lib.definitions()) {
        const bool is_new = std::find(before.begin(), before.end(), name) == before.end();
        const bool from_self = !def.source.empty() && canonString(def.source) == self;
        if (is_new || from_self)
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
