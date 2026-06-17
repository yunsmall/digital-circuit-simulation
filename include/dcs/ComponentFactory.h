// ComponentFactory — 元件类型注册表
//
// 每个元件 .cpp 用 REGISTER_COMPONENT 宏注册元数据 + 创建函数。
// JSON 解析器通过 metas() 获取所有类型的参数列表，读写电路文件。
//
// 生命周期: Meyer's Singleton，首次调用 instance() 时构造，程序结束时析构。
#pragma once
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

class ComponentFactory {
public:
    // 参数描述
    struct ParamMeta {
        std::string name; // 参数名
        std::string default_val; // 默认值字符串（空 = 必填）
        std::vector<std::string> valid_values; // 可选约束：非空时参数只能取列表中的值
    };

    // 元件类型描述
    struct Meta {
        std::string type_name;
        std::vector<ParamMeta> params; // 构造参数
        std::vector<std::string> input_pins; // 引脚名列表（空 = 自动从实例获取）
        std::vector<std::string> output_pins;
    };

    // params: 参数名 → 值的字符串映射，creator 自行解析
    using Creator = std::function<std::unique_ptr<Component>(
            const std::string &name, const std::unordered_map<std::string, std::string> &params)>;

    static ComponentFactory &instance(); // 实现在 ComponentFactory.cpp，保证此 .obj 被链接

    void registerType(Meta meta, Creator creator) {
        _creators[meta.type_name] = std::move(creator);
        _metas[meta.type_name] = std::move(meta);
    }

    // 校验失败时返回错误信息；成功时 error 为空
    static std::string validateParams(const Meta &meta, const std::unordered_map<std::string, std::string> &params) {
        for (auto &pm: meta.params) {
            if (pm.valid_values.empty())
                continue;
            auto it = params.find(pm.name);
            if (it == params.end())
                continue; // 缺省用默认值，不校验
            if (std::find(pm.valid_values.begin(), pm.valid_values.end(), it->second) == pm.valid_values.end()) {
                std::string valid;
                for (size_t i = 0; i < pm.valid_values.size(); i++)
                    valid += (i ? ", " : "") + pm.valid_values[i];
                return std::format("参数 {} 的值 \"{}\" 无效，允许: {}", pm.name, it->second, valid);
            }
        }
        return "";
    }

    std::unique_ptr<Component> create(const std::string &type_name, const std::string &name,
                                      const std::unordered_map<std::string, std::string> &params,
                                      std::string &error) const {
        auto it = _creators.find(type_name);
        if (it == _creators.end()) {
            error = std::format("未知元件类型: {}", type_name);
            return nullptr;
        }
        auto mi = _metas.find(type_name);
        if (mi != _metas.end()) {
            error = validateParams(mi->second, params);
            if (!error.empty())
                return nullptr;
        }
        return it->second(name, params);
    }

    // 无校验版本（C++ API 直接调用，跳过 valid_values 检查）
    std::unique_ptr<Component> createUnchecked(const std::string &type_name, const std::string &name,
                                               const std::unordered_map<std::string, std::string> &params) const {
        auto it = _creators.find(type_name);
        if (it == _creators.end())
            return nullptr;
        return it->second(name, params);
    }

    const auto &metas() const {
        return _metas;
    }
    bool hasType(const std::string &t) const {
        return _metas.find(t) != _metas.end();
    }

private:
    ComponentFactory() = default;
    std::unordered_map<std::string, Creator> _creators;
    std::unordered_map<std::string, Meta> _metas;
};

// ---- 注册宏 ----
// 用法：在 .cpp 文件全局作用域中
//   REGISTER_COMPONENT("and", "and", LogicGate,
//       {{"inputs", "2"}, {"bit_width", "8"}},
//       {{"in0","in1"}, {"out"}})
//
// 参数: type_name, 元数据 params, in/out 引脚名, 类名, 构造参数...
#define REGISTER_COMPONENT_META(type_name, class_name, meta_params, in_pins, out_pins, ...)                            \
    namespace {                                                                                                        \
        struct _Reg_##class_name {                                                                                     \
            _Reg_##class_name() {                                                                                      \
                dsc::ComponentFactory::Meta _m;                                                                        \
                _m.type_name = type_name;                                                                              \
                _m.params = meta_params;                                                                               \
                _m.input_pins = in_pins;                                                                               \
                _m.output_pins = out_pins;                                                                             \
                dsc::ComponentFactory::instance().registerType(                                                        \
                        std::move(_m),                                                                                 \
                        [](const std::string &_n, const std::unordered_map<std::string, std::string> &_p) {            \
                            return std::make_unique<class_name>(_n, ##__VA_ARGS__);                                    \
                        });                                                                                            \
            }                                                                                                          \
        } _reg_##class_name;                                                                                           \
    }

} // namespace dsc
