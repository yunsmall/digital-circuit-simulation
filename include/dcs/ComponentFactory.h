// ComponentFactory — 元件类型注册表
//
// 元件在自己的 .cpp 里用 DCS_REGISTER_COMPONENT 自注册（见文件末尾的宏），
// 类型名、参数、引脚、分类、描述和实现放在一起，新增元件不用再改中央表。
// JSON 解析器通过 metas() 获取所有类型的参数列表，读写电路文件。
//
// 生命周期: Meyer's Singleton，首次调用 instance() 时构造，程序结束时析构。
#pragma once
#include <charconv>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

// 整型参数解析：JSON 里把整型参数写成非数字时不能抛异常打掉整个进程，
// 这里退回默认值，真正的非法值由 validateParams 和元件构造后的参数校验收口
inline int paramInt(const std::unordered_map<std::string, std::string> &p, const std::string &k, int d) {
    auto it = p.find(k);
    if (it == p.end())
        return d;
    const auto &s = it->second;
    int v = d;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return (ec == std::errc{} && ptr == s.data() + s.size()) ? v : d;
}

inline std::string paramStr(const std::unordered_map<std::string, std::string> &p, const std::string &k,
                            const std::string &d) {
    auto it = p.find(k);
    return it != p.end() ? it->second : d;
}

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

// 分类名称（供 CLI / 外部工具展示）
// 英文分类键：给机器读取（--list-types --json、外部工具），不受显示语言影响
inline const char *categoryKey(ComponentCategory c) {
    switch (c) {
        case ComponentCategory::Gate:       return "gate";
        case ComponentCategory::Reduction:  return "reduction";
        case ComponentCategory::Arithmetic: return "arithmetic";
        case ComponentCategory::Sequential: return "sequential";
        case ComponentCategory::Float:      return "float";
        case ComponentCategory::Memory:     return "memory";
        case ComponentCategory::DataPath:   return "datapath";
        case ComponentCategory::Bus:        return "bus";
        case ComponentCategory::CPU:        return "cpu";
        case ComponentCategory::Misc:       return "misc";
    }
    return "unknown";
}

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
        // 分类 key。内置分类用 ComponentCategory 枚举，第三方元件可以直接给字符串，
        // 再用 registerCategory() 登记显示名，不需要改本库的枚举
        std::string category = "misc";
        std::string description; // 元件功能简介（空 = 未填写）

        Meta() = default;
        Meta(std::string t, std::vector<ParamMeta> ps, std::vector<std::string> in,
             std::vector<std::string> out, ComponentCategory c, std::string desc) :
            type_name(std::move(t)), params(std::move(ps)), input_pins(std::move(in)),
            output_pins(std::move(out)), category(categoryKey(c)), description(std::move(desc)) {
        }
        Meta(std::string t, std::vector<ParamMeta> ps, std::vector<std::string> in,
             std::vector<std::string> out, std::string c, std::string desc) :
            type_name(std::move(t)), params(std::move(ps)), input_pins(std::move(in)),
            output_pins(std::move(out)), category(std::move(c)), description(std::move(desc)) {
        }
    };

    // params: 参数名 → 值的字符串映射，creator 自行解析
    using Creator = std::function<std::unique_ptr<Component>(
            const std::string &name, const std::unordered_map<std::string, std::string> &params)>;

    static ComponentFactory &instance(); // 实现在 ComponentFactory.cpp，保证此 .obj 被链接

    // 分类表：内置分类在单例构造时登记；第三方元件可以注册自己的分类，
    // 不需要改动本库的枚举。未登记的分类直接按 key 显示
    void registerCategory(const std::string &key, const std::string &display_name) {
        _categories[key] = display_name;
    }
    [[nodiscard]] std::string categoryDisplayName(const std::string &key) const {
        auto it = _categories.find(key);
        return it == _categories.end() ? key : it->second;
    }

    void registerType(Meta meta, Creator creator) {
        _creators[meta.type_name] = std::move(creator);
        _metas[meta.type_name] = std::move(meta);
    }

    // 校验失败时返回错误信息；成功时 error 为空
    static std::string validateParams(const Meta &meta, const std::unordered_map<std::string, std::string> &params) {
        // 默认值是整数的参数，用户给的值也必须是整数：paramInt 解析失败
        // 会静默回退默认值，不拦住的话 "bit_width": "abc" 会按默认位宽跑。
        // 解析规则要和 Creator 一致：Constant 的 value 支持 0x/0b 前缀，
        // 用 64 位整数是因为 wb_dcache 的 base 默认值 2^31 超出 int32
        auto is_int = [](const std::string &s) {
            std::string_view v(s);
            int base = 10;
            if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
                v.remove_prefix(2);
                base = 16;
            } else if (v.size() > 2 && v[0] == '0' && (v[1] == 'b' || v[1] == 'B')) {
                v.remove_prefix(2);
                base = 2;
            }
            int64_t out = 0;
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
            return !v.empty() && ec == std::errc{} && ptr == v.data() + v.size();
        };
        for (auto &pm: meta.params) {
            auto it = params.find(pm.name);
            if (it == params.end())
                continue; // 缺省用默认值，不校验
            if (!pm.valid_values.empty()) {
                if (std::find(pm.valid_values.begin(), pm.valid_values.end(), it->second) == pm.valid_values.end()) {
                    std::string valid;
                    for (size_t i = 0; i < pm.valid_values.size(); i++)
                        valid += (i ? ", " : "") + pm.valid_values[i];
                    return std::format("参数 {} 的值 \"{}\" 无效，允许: {}", pm.name, it->second, valid);
                }
            } else if (is_int(pm.default_val) && !is_int(it->second)) {
                return std::format("参数 {} 必须是整数，给定 \"{}\"", pm.name, it->second);
            }
        }
        return "";
    }

    std::unique_ptr<Component> create(const std::string &type_name, const std::string &name,
                                      const std::unordered_map<std::string, std::string> &params,
                                      std::string &error) const {
        return createImpl(type_name, name, params, true, &error);
    }

    // 无校验版本（C++ API 直接调用，跳过 valid_values 检查）
    std::unique_ptr<Component> createUnchecked(const std::string &type_name, const std::string &name,
                                               const std::unordered_map<std::string, std::string> &params,
                                               std::string *error = nullptr) const {
        return createImpl(type_name, name, params, false, error);
    }

    const auto &metas() const {
        return _metas;
    }
    bool hasType(const std::string &t) const {
        return _metas.find(t) != _metas.end();
    }

private:
    // create/createUnchecked 共用：一次查找、一次异常转换；validate=false 时跳过
    // valid_values（C++ API 直接构造的元件允许传内部值）
    std::unique_ptr<Component> createImpl(const std::string &type_name, const std::string &name,
                                          const std::unordered_map<std::string, std::string> &params,
                                          bool validate, std::string *error) const {
        // 成功时也要把调用方传进来的旧错误清掉（调用方常复用同一个字符串）
        if (error)
            error->clear();
        auto it = _creators.find(type_name);
        if (it == _creators.end()) {
            if (error)
                *error = std::format("未知元件类型: {}", type_name);
            return nullptr;
        }
        if (validate) {
            auto mi = _metas.find(type_name);
            if (mi != _metas.end()) {
                std::string err = validateParams(mi->second, params);
                if (!err.empty()) {
                    if (error)
                        *error = std::move(err);
                    return nullptr;
                }
            }
        }
        // 元件构造函数的参数校验会抛 std::invalid_argument。对直接 new 的 C++ 用户
        // 那是开发期错误，保留；但 JSON/UI 传进来的参数是用户输入，必须转成 error，
        // 不能让异常一路穿到 main 打掉进程
        try {
            return it->second(name, params);
        } catch (const std::exception &e) {
            if (error)
                *error = std::format("创建元件 '{}' 失败: {}", name, e.what());
            return nullptr;
        }
    }

    std::unordered_map<std::string, Creator> _creators;
    std::unordered_map<std::string, Meta> _metas;
    std::map<std::string, std::string> _categories;

    ComponentFactory() {
        const ComponentCategory builtin[] = {
                ComponentCategory::Gate,       ComponentCategory::Reduction,
                ComponentCategory::Arithmetic, ComponentCategory::Sequential,
                ComponentCategory::Float,      ComponentCategory::Memory,
                ComponentCategory::DataPath,   ComponentCategory::Bus,
                ComponentCategory::CPU,        ComponentCategory::Misc,
        };
        for (auto c: builtin)
            _categories[categoryKey(c)] = categoryName(c);
    }
};

// 元件自注册：在元件的 .cpp 里写一次，程序启动时自动登记到工厂。
// 工厂是 Meyer's Singleton，静态初始化顺序安全。
// 用可变参数宏而不是两个固定参数：Meta 的初始化列表里全是花括号内的逗号，
// 预处理器只认圆括号保护，固定参数宏会在那里被拆成"参数过多"
// 注意：静态库中未被引用的目标文件会被链接器整个丢掉，注册对象也就不会执行，
// 所以 CMake 对静态库场景统一加了 WHOLE_ARCHIVE（见 CMakeLists.txt）
inline bool registerComponentAtStartup(ComponentFactory::Meta meta, ComponentFactory::Creator creator) {
    ComponentFactory::instance().registerType(std::move(meta), std::move(creator));
    return true;
}

} // namespace dsc

#define DCS_REGISTER_IMPL2(a, b) a##b
#define DCS_REGISTER_IMPL1(a, b) DCS_REGISTER_IMPL2(a, b)
#define DCS_REGISTER_COMPONENT(...)                                                    \
    namespace {                                                                        \
    [[maybe_unused]] const bool DCS_REGISTER_IMPL1(dcs_registered_, __LINE__) =        \
            ::dsc::registerComponentAtStartup(__VA_ARGS__);                            \
    }

