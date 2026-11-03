// CircuitLibrary — 共享蓝图注册表实现
#include "dcs/CircuitLibrary.h"
#include <format>
#include <nlohmann/json.hpp>
#include "dcs/ComponentFactory.h"
#include "dcs/Context.h"

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
    _search_paths.push_back(dir);
}

std::string CircuitLibrary::exportJson() const {
    using json = nlohmann::json;
    json root = json::object();

    for (auto &[name, def]: _defs) {
        json jd;

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
        json jin = json::object();
        for (auto &[pin, net]: def.input_expose)
            jin[pin] = net;
        json jout = json::object();
        for (auto &[pin, net]: def.output_expose)
            jout[pin] = net;
        jexp["inputs"] = jin;
        jexp["outputs"] = jout;
        jd["expose"] = jexp;

        root[name] = jd;
    }

    return root.dump();
}

// 参数键的拍平规则与顶层电路装载（CircuitSerializer::flattenParams）保持一致：
// 嵌套对象变成 "backend_opts.hostfwd" 这样的点分键，其它值转成字符串
static void flattenParams(const nlohmann::json &j, const std::string &prefix,
                          std::unordered_map<std::string, std::string> &out) {
    for (auto &[k, v]: j.items()) {
        const std::string key = prefix.empty() ? k : prefix + "." + k;
        if (v.is_object())
            flattenParams(v, key, out);
        else
            out[key] = v.is_string() ? v.get<std::string>() : v.dump();
    }
}

void CircuitLibrary::importJson(const std::string &json_str, std::string &error, const std::string &source) {
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
                if (jexp.contains("inputs")) {
                    if (!jexp["inputs"].is_object()) {
                        error = std::format("定义 '{}' 的 expose.inputs 必须是对象", name);
                        return;
                    }
                    for (auto &[pin, jnet]: jexp["inputs"].items()) {
                        if (!jnet.is_string()) {
                            error = std::format("定义 '{}' 的 expose.inputs['{}'] 必须是字符串", name, pin);
                            return;
                        }
                        def.input_expose[pin] = jnet.get<std::string>();
                    }
                }
                if (jexp.contains("outputs")) {
                    if (!jexp["outputs"].is_object()) {
                        error = std::format("定义 '{}' 的 expose.outputs 必须是对象", name);
                        return;
                    }
                    for (auto &[pin, jnet]: jexp["outputs"].items()) {
                        if (!jnet.is_string()) {
                            error = std::format("定义 '{}' 的 expose.outputs['{}'] 必须是字符串", name, pin);
                            return;
                        }
                        def.output_expose[pin] = jnet.get<std::string>();
                    }
                }
                // 复合元件的引脚按名字连接；同名输入/输出会让连接目标产生歧义
                for (auto &[pin, net]: def.input_expose) {
                    (void) net;
                    if (def.output_expose.count(pin)) {
                        error = std::format("定义 '{}' 的引脚 '{}' 同时出现在 expose.inputs 和 expose.outputs",
                                            name, pin);
                        return;
                    }
                }
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
