#include "dcs/CircuitSerializer.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"
#include "dcs/Context.h"
#include "dcs/util/ParamFlatten.h"

namespace dsc {

namespace {

bool readWholeFile(const std::filesystem::path &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 读取 $meta：kind 缺省为 circuit，format 缺省为 1；比本程序新的大版本直接拒绝。
// 元数据本身是可选的，老文件（没有 $meta）按电路处理
bool readMeta(const nlohmann::json &root, std::string &kind, std::string &error) {
    kind = "circuit";
    auto it = root.find("$meta");
    if (it == root.end())
        return true;
    if (!it->is_object()) {
        error = "$meta 必须是对象";
        return false;
    }
    if (auto k = it->find("kind"); k != it->end()) {
        if (!k->is_string()) {
            error = "$meta.kind 必须是字符串（circuit 或 blueprint）";
            return false;
        }
        kind = k->get<std::string>();
    }
    if (auto f = it->find("format"); f != it->end()) {
        if (!f->is_number_integer()) {
            error = "$meta.format 必须是整数";
            return false;
        }
        const int version = f->get<int>();
        if (version < 1 || version > kJsonFormatVersion) {
            error = std::format("JSON 格式版本 {} 不受支持（本程序支持 1~{}），请升级 dcs", version,
                                kJsonFormatVersion);
            return false;
        }
    }
    // 这些字段会直接写进 Circuit 元数据，类型不对时在这里就报出来，
    // 不要等到 setMetadata 的 value() 抛异常
    for (const char *key: {"name", "author", "description"}) {
        if (auto v = it->find(key); v != it->end() && !v->is_string()) {
            error = std::format("$meta.{} 必须是字符串", key);
            return false;
        }
    }
    return true;
}

// 蓝图路径解析：绝对路径直接用；相对路径先按引用它的 JSON 所在目录找，
// 再按 --lib-path 给的目录顺序找
std::filesystem::path resolveLibPath(const std::string &p, const std::filesystem::path &base,
                                     const CircuitLibrary &lib) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path rel(p);
    if (rel.is_absolute())
        return fs::exists(rel, ec) ? rel : fs::path{};
    if (auto cand = base / rel; fs::exists(cand, ec))
        return cand;
    for (auto &dir: lib.searchPaths()) {
        if (auto cand = fs::path(dir) / rel; fs::exists(cand, ec))
            return cand;
    }
    return {};
}

bool importBlueprintFile(const std::filesystem::path &file, const std::string &key,
                         CircuitLibrary &lib, std::vector<std::string> &stack, std::string &error);

// definitions 对象：值是对象 → 内联定义；值是字符串 → 相对路径引用蓝图 JSON
bool importDefinitions(const nlohmann::json &defs, const std::filesystem::path &base,
                       CircuitLibrary &lib, std::vector<std::string> &stack, std::string &error,
                       const std::string &inline_source) {
    using json = nlohmann::json;
    if (!defs.is_object()) {
        error = "definitions 必须是对象（定义名 → 定义对象或蓝图文件路径）";
        return false;
    }
    // 文件引用先导入、内联定义后导入：同名时以本文件里的内联定义为准
    json inline_defs = json::object();
    for (auto &[key, val]: defs.items()) {
        if (val.is_string()) {
            const auto file = resolveLibPath(val.get<std::string>(), base, lib);
            if (file.empty()) {
                error = std::format("找不到蓝图文件 '{}'（键 '{}'）：相对路径按引用文件所在目录和 "
                                    "--lib-path 查找",
                                    val.get<std::string>(), key);
                return false;
            }
            if (!importBlueprintFile(file, key, lib, stack, error))
                return false;
        } else if (val.is_object()) {
            inline_defs[key] = val;
        } else {
            error = std::format("definitions['{}'] 必须是定义对象或蓝图文件路径", key);
            return false;
        }
    }
    if (!inline_defs.empty()) {
        std::string import_error;
        lib.importJson(inline_defs.dump(), import_error, inline_source, base.string());
        if (!import_error.empty()) {
            error = import_error;
            return false;
        }
    }
    return true;
}

bool importBlueprintFile(const std::filesystem::path &file, const std::string &key,
                         CircuitLibrary &lib, std::vector<std::string> &stack, std::string &error) {
    using json = nlohmann::json;
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string canon = fs::weakly_canonical(file, ec).string();
    if (std::find(stack.begin(), stack.end(), canon) != stack.end()) {
        error = std::format("蓝图引用出现循环: {}", canon);
        return false;
    }
    stack.push_back(canon);

    std::string text;
    if (!readWholeFile(file, text)) {
        error = std::format("读不了蓝图文件: {}", file.string());
        return false;
    }
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error &e) {
        error = std::format("蓝图 JSON 解析失败（{}）: {}", file.string(), e.what());
        return false;
    }

    std::string kind;
    if (!readMeta(root, kind, error)) {
        error = std::format("{}: {}", file.string(), error);
        return false;
    }
    if (kind != "blueprint") {
        error = std::format("{} 不是蓝图 JSON（$meta.kind 应为 \"blueprint\"）", file.string());
        return false;
    }

    if (root.contains("definitions")) {
        if (!importDefinitions(root["definitions"], file.parent_path(), lib, stack, error, file.generic_string()))
            return false;
    } else if (root.contains("components")) {
        // 单定义蓝图：定义内容直接写在根上，名字取 $meta.name，没写就用引用处的键。
        // 一个文件一个元件时用这种形式最省事
        json def = root;
        def.erase("$meta");
        std::string name = key;
        if (auto m = root.find("$meta"); m != root.end() && m->contains("name") && (*m)["name"].is_string())
            name = (*m)["name"].get<std::string>();
        json wrap = json::object();
        wrap[name] = def;
        std::string import_error;
        lib.importJson(wrap.dump(), import_error, file.generic_string(), file.parent_path().string());
        if (!import_error.empty()) {
            error = import_error;
            return false;
        }
    } else {
        error = std::format("蓝图 JSON 里既没有 definitions 也没有 components: {}", file.string());
        return false;
    }
    stack.pop_back();
    return true;
}

// 参数值除了标量，还可以是嵌套对象（后端私有选项之类）。拍平规则统一在
// dcs/util/ParamFlatten.h，顶层电路和蓝图导入必须一致

// 装载失败时把蓝图库恢复成导入前的样子。definitions 导入是逐个 add 的，
// 靠后的蓝图失败时前面的已经写进上下文；不恢复的话后续 --check/--list-definitions
// 会看到一个"半个文件"的库
class LibraryRollback {
public:
    explicit LibraryRollback(CircuitLibrary &lib): _lib(lib), _saved(lib.snapshot()) {}
    ~LibraryRollback() {
        if (!_committed)
            _lib.restore(std::move(_saved));
    }
    void commit() {
        _committed = true;
    }

private:
    CircuitLibrary &_lib;
    std::unordered_map<std::string, CircuitDefinition> _saved;
    bool _committed = false;
};

} // namespace

std::unique_ptr<Circuit> loadCircuitFromJson(const std::string &json_str, std::string &error,
                                           const std::string &base_dir, Context *context) {
    using json = nlohmann::json;

    json root;
    try {
        root = json::parse(json_str);
    } catch (const json::parse_error &e) {
        error = std::format("JSON 解析失败: {}", e.what());
        return nullptr;
    }
    if (!root.is_object()) {
        error = "电路 JSON 的根必须是对象";
        return nullptr;
    }

    // 导入的定义要么全部生效（装载成功），要么全部不生效（失败回滚）
    Context &ctx = context ? *context : Context::global();
    LibraryRollback library_rollback(ctx.library());

    // 整个装载过程都在 try 里：nlohmann 的 get()/value() 遇到类型不符会抛异常，
    // 而这里的 JSON 直接来自用户文件，字段写错只能报错，不能把进程带走
    try {
        // 元数据：区分电路/蓝图，检查格式版本。蓝图文件要由顶层电路的 definitions 引用
        std::string kind;
        if (!readMeta(root, kind, error))
            return nullptr;
        if (kind == "blueprint") {
            error = "这是蓝图 JSON（$meta.kind=blueprint），不能直接当电路加载；"
                    "应放到顶层电路的 definitions 里引用";
            return nullptr;
        }
        if (kind != "circuit") {
            error = std::format("未知的 $meta.kind: '{}'（应为 circuit 或 blueprint）", kind);
            return nullptr;
        }

        auto circuit = std::make_unique<Circuit>();
        if (context)
            circuit->setContext(*context);
        if (auto m = root.find("$meta"); m != root.end())
            circuit->setMetadata(m->value("author", std::string{}), m->value("description", std::string{}));

        // 1. 创建线网，记录名字→Net*
        std::unordered_map<std::string, Net *> net_map;
        if (root.contains("nets")) {
            if (!root["nets"].is_array()) {
                error = "nets 必须是数组";
                return nullptr;
            }
            int index = 0;
            for (auto &jn: root["nets"]) {
                if (!jn.is_string()) {
                    error = std::format("nets[{}] 必须是字符串", index);
                    return nullptr;
                }
                std::string name = jn.get<std::string>();
                Net *net = circuit->createNet(name);
                net_map[name] = net;
                index++;
            }
        }

        // 辅助: 获取或创建线网（遇到未知名字自动创建）
        auto get_or_create_net = [&](const std::string &name) -> Net * {
            if (name.empty())
                return nullptr;
            auto it = net_map.find(name);
            if (it != net_map.end())
                return it->second;
            Net *net = circuit->createNet(name);
            net_map[name] = net;
            return net;
        };

        // 2. 导入共享定义（必须在创建实例前）：值可以是内联定义，也可以是蓝图文件路径
        if (root.contains("definitions")) {
            const std::filesystem::path base =
                    base_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(base_dir);
            std::vector<std::string> stack;
            if (!importDefinitions(root["definitions"], base, circuit->context().library(), stack, error, "内联"))
                return nullptr;
        }

        // 3. 创建元件
        if (!root.contains("components") || !root["components"].is_array()) {
            error = "JSON 缺少 components 数组";
            return nullptr;
        }

        // 元件 prepare 里用 Context::resolvePath 解析参数中的相对路径时，
        // 当前目录就是这份 JSON 所在目录；蓝图子元件展开时 Composite 会按
        // 定义自己的 base_dir 再切一次
        Context::ScopedSourceDir source_guard(
                ctx, base_dir.empty() ? std::filesystem::current_path().string() : base_dir);

        auto &factory = ComponentFactory::instance();

        for (size_t ci = 0; ci < root["components"].size(); ci++) {
            auto &jc = root["components"][ci];
            if (!jc.is_object()) {
                error = std::format("components[{}] 必须是对象", ci);
                return nullptr;
            }
            for (const char *key: {"type", "name"}) {
                if (auto it = jc.find(key); it != jc.end() && !it->is_string()) {
                    error = std::format("components[{}].{} 必须是字符串", ci, key);
                    return nullptr;
                }
            }
            std::string type = jc.value("type", "");
            std::string name = jc.value("name", "");

            if (!factory.hasType(type)) {
                error = std::format("未知元件类型: {}", type);
                return nullptr;
            }

            // 收集参数（嵌套对象拍平成 a.b 键）
            std::unordered_map<std::string, std::string> params;
            if (jc.contains("params")) {
                if (!jc["params"].is_object()) {
                    error = std::format("components[{}].params 必须是对象", ci);
                    return nullptr;
                }
                flattenParams(jc["params"], "", params);
            }
            // 元件参数里的相对路径不在这里改写：谁需要文件谁在 prepare(ctx) 里
            // 用 Context::resolvePath 解析（当前目录由加载器设成 JSON 所在目录）

            std::string create_error;
            std::unique_ptr<Component> comp = factory.create(type, name, params, create_error);
            if (!comp) {
                error = create_error.empty() ? std::format("创建元件失败: type={}, name={}", type, name)
                                             : create_error;
                return nullptr;
            }

            CircuitError add_err;
            Component *comp_ptr = circuit->addComponent(std::move(comp), &add_err);
            if (!comp_ptr) {
                error = add_err.message;
                return nullptr;
            }

            // 连接引脚
            if (jc.contains("connections")) {
                if (!jc["connections"].is_object()) {
                    error = std::format("components[{}].connections 必须是对象", ci);
                    return nullptr;
                }
                for (auto &[pin_name, jnet]: jc["connections"].items()) {
                    std::string net_name;
                    if (jnet.is_null())
                        continue; // 悬空引脚跳过
                    else if (jnet.is_string())
                        net_name = jnet.get<std::string>();
                    else {
                        error = std::format("components[{}].connections['{}'] 必须是字符串或 null", ci,
                                            pin_name);
                        return nullptr;
                    }

                    Net *net = get_or_create_net(net_name);
                    if (net) {
                        // 连接失败（引脚不存在/多驱动冲突）必须报出来：忽略返回值会让
                        // JSON 描述的电路少掉几根连线，运行期表现却是"莫名其妙不工作"
                        auto err = circuit->connect(comp_ptr, pin_name, net);
                        if (err.code != ErrorCode::OK) {
                            error = err.message;
                            return nullptr;
                        }
                    }
                }
            }
        }

        library_rollback.commit();
        return circuit;
    } catch (const json::exception &e) {
        error = std::format("JSON 字段类型错误: {}", e.what());
        return nullptr;
    }
}

std::string exportCircuitJson(const Circuit &circuit) {
    using json = nlohmann::json;
    json root;

    // 元数据：kind 区分电路/蓝图，format 是数据格式版本。作者和说明是可选字段，
    // 加载时原样保留、导出时写回，方便脚本和 IDE 识别文件用途
    json meta;
    meta["kind"] = "circuit";
    meta["format"] = kJsonFormatVersion;
    if (!circuit.author().empty())
        meta["author"] = circuit.author();
    if (!circuit.description().empty())
        meta["description"] = circuit.description();
    root["$meta"] = meta;

    // 共享定义库直接嵌进根对象，不再走"导出成字符串再 parse 回来"
    json j_defs = circuit.context().library().toJson();
    if (!j_defs.empty())
        root["definitions"] = j_defs;

    // 线网: 名字数组
    json j_nets = json::array();
    for (auto &n: circuit.nets())
        j_nets.push_back(n->name());

    // 元件
    json j_comps = json::array();
    for (auto &c: circuit.components()) {
        json jc;
        jc["type"] = c->typeName();
        jc["name"] = c->name();

        // 构造参数（复合元件导出 definition 引用）
        auto params = c->exportParams();
        if (!params.empty()) {
            json jp = json::object();
            for (auto &[k, v]: params)
                jp[k] = v;
            jc["params"] = jp;
        }

        // 引脚连接: pin_name → net_name (null 表示悬空)
        json jconn = json::object();
        for (auto &p: c->inputs()) {
            if (p->net())
                jconn[p->name()] = p->net()->name();
            else
                jconn[p->name()] = nullptr;
        }
        for (auto &p: c->outputs()) {
            if (p->net())
                jconn[p->name()] = p->net()->name();
            else
                jconn[p->name()] = nullptr;
        }
        jc["connections"] = jconn;

        j_comps.push_back(jc);
    }

    root["nets"] = j_nets;
    root["components"] = j_comps;
    return root.dump(2);
}

} // namespace dsc
