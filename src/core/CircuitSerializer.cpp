#include "dcs/CircuitSerializer.h"

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"

namespace dsc {

std::unique_ptr<Circuit> loadCircuitFromJson(const std::string &json_str, std::string &error,
                                           const std::string &base_dir) {
    using json = nlohmann::json;

    json root;
    try {
        root = json::parse(json_str);
    } catch (const json::parse_error &e) {
        error = std::format("JSON 解析失败: {}", e.what());
        return nullptr;
    }

    auto circuit = std::make_unique<Circuit>();

    // 1. 创建线网，记录名字→Net*
    std::unordered_map<std::string, Net *> net_map;
    if (root.contains("nets") && root["nets"].is_array()) {
        for (auto &jn: root["nets"]) {
            std::string name = jn.get<std::string>();
            Net *net = circuit->createNet(name);
            net_map[name] = net;
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

    // 2. 导入共享定义（必须在创建实例前）
    if (root.contains("definitions"))
        CircuitLibrary::instance().importJson(root["definitions"].dump(), error);

    // 3. 创建元件
    if (!root.contains("components") || !root["components"].is_array()) {
        error = "JSON 缺少 components 数组";
        return nullptr;
    }

    auto &factory = ComponentFactory::instance();

    for (auto &jc: root["components"]) {
        std::string type = jc.value("type", "");
        std::string name = jc.value("name", "");

        if (!factory.hasType(type)) {
            error = std::format("未知元件类型: {}", type);
            return nullptr;
        }

        // 收集参数
        std::unordered_map<std::string, std::string> params;
        if (jc.contains("params") && jc["params"].is_object()) {
            for (auto &[k, v]: jc["params"].items())
                params[k] = v.is_string() ? v.get<std::string>() : v.dump();
        }
        // DLL 路径相对于 JSON 文件所在目录
        // 带文件路径的元件，其路径相对于 JSON 所在目录解析；
        // 不这样做的话，换个工作目录加载就找不到文件了
        if (!base_dir.empty()) {
            const char *path_key = type == "dll" ? "path" : type == "rom" ? "filepath" : nullptr;
            if (path_key) {
                auto it = params.find(path_key);
                if (it != params.end()) {
                    std::filesystem::path p(it->second);
                    if (p.is_relative())
                        it->second = (std::filesystem::path(base_dir) / p).string();
                }
            }
        }

        std::string create_error;
        std::unique_ptr<Component> comp = factory.create(type, name, params, create_error);
        if (!comp) {
            error = create_error.empty() ? std::format("创建元件失败: type={}, name={}", type, name) : create_error;
            return nullptr;
        }

        CircuitError add_err;
        Component *comp_ptr = circuit->addComponent(std::move(comp), &add_err);
        if (!comp_ptr) {
            error = add_err.message;
            return nullptr;
        }

        // 连接引脚
        if (jc.contains("connections") && jc["connections"].is_object()) {
            for (auto &[pin_name, jnet]: jc["connections"].items()) {
                std::string net_name;
                if (jnet.is_null())
                    continue; // 悬空引脚跳过
                else if (jnet.is_string())
                    net_name = jnet.get<std::string>();
                else
                    continue;

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

    return circuit;
}

} // namespace dsc
