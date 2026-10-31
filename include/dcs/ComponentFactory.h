// ComponentFactory — 元件类型注册表
//
// 所有类型在 InitAllComponents 的构造函数里手写 registerType 注册（见 src/ComponentFactory.cpp）。
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

// 元件分类枚举，供外部工具按类别筛选
enum class ComponentCategory {
    Gate,       // 门电路
    Reduction,  // 归约运算
    Arithmetic, // 算术/比较运算
    Sequential, // 时序逻辑
    Float,      // 浮点运算
    Memory,     // 存储元件
    DataPath,   // 数据通路（Mux/Decoder/移位/合并拆分/符号扩展/延迟线等）
    Bus,        // 总线/互连（Wishbone 等）
    CPU,        // 处理器核
    Misc,       // 杂项（打印/复合/DLL 等）
};

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
        ComponentCategory category = ComponentCategory::Misc;
        std::string description; // 元件功能简介（空 = 未填写）
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
        // 元件构造函数的参数校验会抛 std::invalid_argument。对直接 new 的 C++ 用户
        // 那是开发期错误，保留；但 JSON/UI 传进来的参数是用户输入，必须转成 error，
        // 不能让异常一路穿到 main 打掉进程
        try {
            return it->second(name, params);
        } catch (const std::exception &e) {
            error = std::format("创建元件 '{}' 失败: {}", name, e.what());
            return nullptr;
        }
    }

    // 无校验版本（C++ API 直接调用，跳过 valid_values 检查）
    std::unique_ptr<Component> createUnchecked(const std::string &type_name, const std::string &name,
                                               const std::unordered_map<std::string, std::string> &params,
                                               std::string *error = nullptr) const {
        auto it = _creators.find(type_name);
        if (it == _creators.end()) {
            if (error)
                *error = std::format("未知元件类型: {}", type_name);
            return nullptr;
        }
        try {
            return it->second(name, params);
        } catch (const std::exception &e) {
            if (error)
                *error = std::format("创建元件 '{}' 失败: {}", name, e.what());
            return nullptr;
        }
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

// 分类名称（供 CLI / 外部工具展示）
inline const char *categoryName(ComponentCategory c) {
    switch (c) {
        case ComponentCategory::Gate:       return "门电路";
        case ComponentCategory::Reduction:  return "归约运算";
        case ComponentCategory::Arithmetic: return "算术运算";
        case ComponentCategory::Sequential: return "时序逻辑";
        case ComponentCategory::Float:      return "浮点运算";
        case ComponentCategory::Memory:     return "存储元件";
        case ComponentCategory::DataPath:   return "数据通路";
        case ComponentCategory::Bus:        return "总线互连";
        case ComponentCategory::CPU:        return "处理器核";
        case ComponentCategory::Misc:       return "杂项";
    }
    return "未知";
}

} // namespace dsc
