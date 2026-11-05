// 元件 schema 生成：把 ComponentFactory 里的元数据转成 JSON Schema
#include "dcs/SchemaGen.h"

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

#include "dcs/ComponentFactory.h"

namespace dsc {

std::string componentSchemaJson() {
    using json = nlohmann::json;
    auto &factory = ComponentFactory::instance();

    // 类型名排序，输出稳定（方便把生成结果提交进版本库做 diff）
    std::vector<std::string> type_names;
    for (auto &[name, meta]: factory.metas())
        type_names.push_back(name);
    std::sort(type_names.begin(), type_names.end());

    json one_of = json::array();
    for (auto &name: type_names) {
        const auto &meta = factory.metas().at(name);
        json variant;
        variant["description"] = meta.description;
        variant["properties"] = json::object();
        variant["properties"]["type"] = {{"const", name}};
        variant["properties"]["name"] = {{"type", "string"}};
        variant["required"] = json::array({"type", "name"});

        if (!meta.params.empty()) {
            json props = json::object();
            for (auto &p: meta.params) {
                json prop;
                prop["type"] = "string"; // 参数值在 JSON 里统一是字符串或标量
                if (!p.default_val.empty())
                    prop["default"] = p.default_val;
                if (!p.valid_values.empty())
                    prop["enum"] = p.valid_values;
                props[p.name] = prop;
            }
            variant["properties"]["params"] = {
                    {"type", "object"},
                    {"properties", props},
                    // 和工厂的参数校验保持一致：不允许额外参数的元件，schema 也要
                    // 标红拼错的键，否则 IDE 显示合法、加载时才报"未知参数"
                    {"additionalProperties", meta.allow_extra_params},
            };
        }
        one_of.push_back(variant);
    }

    json root;
    root["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    root["$id"] = "https://github.com/yunsmall/digital-circuit-simulation/schemas/component.schema.json";
    root["title"] = "dcs 元件";
    root["description"] = "由 dcs --dump-schema 从元件注册表生成，供 IDE 提示；不参与运行期校验";
    root["oneOf"] = one_of;
    return root.dump(2);
}

} // namespace dsc
