// CircuitLibrary — 共享蓝图注册表实现
#include "dcs/CircuitLibrary.h"
#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>
#include "dcs/ComponentFactory.h"
#include "dcs/Context.h"
#include "dcs/util/ParamFlatten.h"

namespace dsc {

CircuitLibrary &CircuitLibrary::instance() {
    // 进程级默认上下文里的库；显式建 Context 的调用方直接用自己的那份
    return Context::global().library();
}

void CircuitLibrary::add(CircuitDefinition def) {
    def.import_serial = ++_import_counter;
    _defs[def.name] = std::move(def);
}

const CircuitDefinition *CircuitLibrary::find(const std::string &name) const {
    auto it = _defs.find(name);
    return it != _defs.end() ? &it->second : nullptr;
}

bool CircuitLibrary::exists(const std::string &name) const {
    return _defs.find(name) != _defs.end();
}

void CircuitLibrary::addSearchPath(const std::string &dir) {
    if (dir.empty())
        return;
    // --lib-path 和 DCS_LIB_PATH 可能给出同一个目录；重复项会让每次解析蓝图
    // 都多做一遍无意义的文件系统探测，顺序上也没有任何作用
    if (std::find(_search_paths.begin(), _search_paths.end(), dir) != _search_paths.end())
        return;
    _search_paths.push_back(dir);
}

nlohmann::json CircuitLibrary::toJson() const {
    using json = nlohmann::json;
    json root = json::object();

    for (auto &[name, def]: _defs) {
        json jd;

        if (!def.params.empty()) {
            json jp = json::object();
            for (auto &[k, v]: def.params)
                jp[k] = v;
            jd["params"] = jp;
        }

        // 线网
        json jnets = json::array();
        for (auto &n: def.nets)
            jnets.push_back(n);
        jd["nets"] = jnets;

        // 元件
        json jcomps = json::array();
        for (auto &cr: def.components) {
            json jc;
            jc["type"] = cr.type_name;
            jc["name"] = cr.instance_name;

            if (!cr.params.empty()) {
                json jp = json::object();
                for (auto &[k, v]: cr.params)
                    jp[k] = v;
                jc["params"] = jp;
            }

            json jconn = json::object();
            for (auto &[pin, net]: cr.connections)
                jconn[pin] = net;
            jc["connections"] = jconn;

            jcomps.push_back(jc);
        }
        jd["components"] = jcomps;

        // expose
        json jexp;
        auto emit_expose = [](const std::map<std::string, ExposedPin> &m) {
            json j = json::object();
            for (auto &[pin, ep]: m) {
                if (ep.width.empty())
                    j[pin] = ep.net;
                else
                    j[pin] = {{"net", ep.net}, {"width", ep.width}};
            }
            return j;
        };
        jexp["inputs"] = emit_expose(def.input_expose);
        jexp["outputs"] = emit_expose(def.output_expose);
        jd["expose"] = jexp;

        root[name] = jd;
    }

    return root;
}

std::string CircuitLibrary::exportJson() const {
    return toJson().dump();
}

// 参数键的拍平规则统一在 dcs/util/ParamFlatten.h，与顶层电路装载共用一份实现

void CircuitLibrary::importJson(const std::string &json_str, std::string &error, const std::string &source,
                                const std::string &base_dir) {
    using json = nlohmann::json;

    json root;
    try {
        root = json::parse(json_str);
    } catch (const json::parse_error &e) {
        error = std::format("定义库 JSON 解析失败: {}", e.what());
        return;
    }

    if (!root.is_object()) {
        error = "定义库 JSON 的根必须是对象";
        return;
    }

    // 字段类型不对时 convert 会抛异常；这里统一转成可读错误，避免集成方崩溃。
    // 先全部解析成功再登记，报错时不会留下半个定义库
    std::vector<CircuitDefinition> parsed;
    try {
        for (auto &[name, jd]: root.items()) {
            if (!jd.is_object()) {
                error = std::format("定义 '{}' 必须是对象", name);
                return;
            }
            CircuitDefinition def;
            def.name = name;
            def.source = source;
            def.base_dir = base_dir;

            // 蓝图参数默认值：都是字符串，实例可以按名字覆盖
            if (jd.contains("params")) {
                if (!jd["params"].is_object()) {
                    error = std::format("定义 '{}' 的 params 必须是对象", name);
                    return;
                }
                for (auto &[k, v]: jd["params"].items()) {
                    if (!v.is_string()) {
                        error = std::format("定义 '{}' 的参数 '{}' 必须是字符串", name, k);
                        return;
                    }
                    def.params[k] = v.get<std::string>();
                }
            }

            // 线网
            if (jd.contains("nets")) {
                if (!jd["nets"].is_array()) {
                    error = std::format("定义 '{}' 的 nets 必须是数组", name);
                    return;
                }
                for (auto &jn: jd["nets"]) {
                    if (!jn.is_string()) {
                        error = std::format("定义 '{}' 的 nets 元素必须是字符串", name);
                        return;
                    }
                    def.nets.push_back(jn.get<std::string>());
                }
            }

            // 元件
            if (jd.contains("components")) {
                if (!jd["components"].is_array()) {
                    error = std::format("定义 '{}' 的 components 必须是数组", name);
                    return;
                }
                for (auto &jc: jd["components"]) {
                    if (!jc.is_object()) {
                        error = std::format("定义 '{}' 的 components 元素必须是对象", name);
                        return;
                    }
                    ComponentRecipe cr;
                    for (const char *key: {"type", "name"}) {
                        if (auto it = jc.find(key); it != jc.end() && !it->is_string()) {
                            error = std::format("定义 '{}' 的元件 {} 必须是字符串", name, key);
                            return;
                        }
                    }
                    cr.type_name = jc.value("type", "");
                    cr.instance_name = jc.value("name", "");

                    if (jc.contains("params")) {
                        if (!jc["params"].is_object()) {
                            error = std::format("定义 '{}' 的元件 params 必须是对象", name);
                            return;
                        }
                        // 和顶层电路一致：嵌套对象拍平成 "a.b" 键，否则蓝图里的
                        // backend_opts 这类参数会整块漏给元件
                        flattenParams(jc["params"], "", cr.params);
                    }

                    if (jc.contains("connections")) {
                        if (!jc["connections"].is_object()) {
                            error = std::format("定义 '{}' 的元件 connections 必须是对象", name);
                            return;
                        }
                        for (auto &[pin, jnet]: jc["connections"].items()) {
                            if (jnet.is_null())
                                continue;   // null 表示悬空
                            if (!jnet.is_string()) {
                                error = std::format("定义 '{}' 的 connections['{}'] 必须是字符串或 null",
                                                    name, pin);
                                return;
                            }
                            cr.connections[pin] = jnet.get<std::string>();
                        }
                    }

                    def.components.push_back(std::move(cr));
                }
            }

            // expose
            if (jd.contains("expose")) {
                if (!jd["expose"].is_object()) {
                    error = std::format("定义 '{}' 的 expose 必须是对象", name);
                    return;
                }
                auto &jexp = jd["expose"];
                auto parse_expose = [&](const nlohmann::json &obj, std::map<std::string, ExposedPin> &out,
                                        const char *which) {
                    for (auto &[pin, jv]: obj.items()) {
                        ExposedPin ep;
                        if (jv.is_string()) {
                            ep.net = jv.get<std::string>();
                        } else if (jv.is_object()) {
                            if (!jv.contains("net") || !jv["net"].is_string()) {
                                error = std::format("定义 '{}' 的 expose.{}['{}'].net 必须是字符串", name, which, pin);
                                return false;
                            }
                            ep.net = jv["net"].get<std::string>();
                            if (jv.contains("width")) {
                                if (!jv["width"].is_string()) {
                                    error = std::format("定义 '{}' 的 expose.{}['{}'].width 必须是字符串", name,
                                                        which, pin);
                                    return false;
                                }
                                ep.width = jv["width"].get<std::string>();
                            }
                        } else {
                            error = std::format("定义 '{}' 的 expose.{}['{}'] 必须是线网名或 {{net,width}}", name,
                                                which, pin);
                            return false;
                        }
                        out[pin] = std::move(ep);
                    }
                    return true;
                };
                if (jexp.contains("inputs")) {
                    if (!jexp["inputs"].is_object()) {
                        error = std::format("定义 '{}' 的 expose.inputs 必须是对象", name);
                        return;
                    }
                    if (!parse_expose(jexp["inputs"], def.input_expose, "inputs"))
                        return;
                }
                if (jexp.contains("outputs")) {
                    if (!jexp["outputs"].is_object()) {
                        error = std::format("定义 '{}' 的 expose.outputs 必须是对象", name);
                        return;
                    }
                    if (!parse_expose(jexp["outputs"], def.output_expose, "outputs"))
                        return;
                }
                // 复合元件的引脚按名字连接；同名输入/输出会让连接目标产生歧义
                for (auto &[pin, ep]: def.input_expose) {
                    (void) ep;
                    if (def.output_expose.count(pin)) {
                        error = std::format("定义 '{}' 的引脚 '{}' 同时出现在 expose.inputs 和 expose.outputs",
                                            name, pin);
                        return;
                    }
                }
                // 输入和输出暴露到同一个内部线网时，net_map 会被后者覆盖，
                // 先处理的引脚对应的外部线网静默悬空；同方向重复也一样，
                // 所以两个方向的内部线网名必须整体互不相同
                std::map<std::string, std::string> net_owner;
                auto claim_nets = [&](const std::map<std::string, ExposedPin> &m, const char *which) -> bool {
                    for (auto &[pin, ep]: m) {
                        const auto full = std::format("{}.{}", which, pin);
                        auto [it, inserted] = net_owner.emplace(ep.net, full);
                        if (!inserted) {
                            error = std::format("定义 '{}' 的 expose 引脚 '{}' 和 '{}' 指向同一个内部线网 '{}'",
                                                name, it->second, full, ep.net);
                            return false;
                        }
                    }
                    return true;
                };
                if (!claim_nets(def.input_expose, "inputs") || !claim_nets(def.output_expose, "outputs"))
                    return;
            }

            parsed.push_back(std::move(def));
        }
    } catch (const json::exception &e) {
        error = std::format("定义库字段类型错误: {}", e.what());
        return;
    }
    for (auto &def: parsed)
        add(std::move(def));
}

} // namespace dsc
