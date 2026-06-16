// CircuitLibrary — 共享蓝图注册表实现
#include "dcs/CircuitLibrary.h"
#include <format>
#include <nlohmann/json.hpp>
#include "dcs/ComponentFactory.h"

namespace dsc {

CircuitLibrary &CircuitLibrary::instance() {
    static CircuitLibrary lib;
    return lib;
}

void CircuitLibrary::add(CircuitDefinition def) {
    _defs[def.name] = std::move(def);
}

const CircuitDefinition *CircuitLibrary::find(const std::string &name) const {
    auto it = _defs.find(name);
    return it != _defs.end() ? &it->second : nullptr;
}

bool CircuitLibrary::exists(const std::string &name) const {
    return _defs.find(name) != _defs.end();
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

void CircuitLibrary::importJson(const std::string &json_str, std::string &error) {
    using json = nlohmann::json;

    json root;
    try {
        root = json::parse(json_str);
    } catch (const json::parse_error &e) {
        error = std::format("定义库 JSON 解析失败: {}", e.what());
        return;
    }

    if (!root.is_object())
        return;

    for (auto &[name, jd]: root.items()) {
        CircuitDefinition def;
        def.name = name;

        // 线网
        if (jd.contains("nets") && jd["nets"].is_array())
            for (auto &jn: jd["nets"])
                def.nets.push_back(jn.get<std::string>());

        // 元件
        if (jd.contains("components") && jd["components"].is_array()) {
            for (auto &jc: jd["components"]) {
                ComponentRecipe cr;
                cr.type_name = jc.value("type", "");
                cr.instance_name = jc.value("name", "");

                if (jc.contains("params") && jc["params"].is_object())
                    for (auto &[k, v]: jc["params"].items())
                        cr.params[k] = v.is_string() ? v.get<std::string>() : v.dump();

                if (jc.contains("connections") && jc["connections"].is_object())
                    for (auto &[pin, jnet]: jc["connections"].items())
                        if (jnet.is_string())
                            cr.connections[pin] = jnet.get<std::string>();

                def.components.push_back(std::move(cr));
            }
        }

        // expose
        if (jd.contains("expose") && jd["expose"].is_object()) {
            auto &jexp = jd["expose"];
            if (jexp.contains("inputs") && jexp["inputs"].is_object())
                for (auto &[pin, jnet]: jexp["inputs"].items())
                    def.input_expose[pin] = jnet.get<std::string>();
            if (jexp.contains("outputs") && jexp["outputs"].is_object())
                for (auto &[pin, jnet]: jexp["outputs"].items())
                    def.output_expose[pin] = jnet.get<std::string>();
        }

        add(std::move(def));
    }
}

} // namespace dsc
