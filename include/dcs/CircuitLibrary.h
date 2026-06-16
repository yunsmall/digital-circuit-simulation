// CircuitLibrary — 复合元件共享蓝图注册表
//
// 多个 CompositeComponent 实例可共享同一个 CircuitDefinition，
// JSON 导出时定义只出现一次，实例引用定义名即可。
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dsc {

// 子元件创建配方
struct ComponentRecipe {
    std::string type_name;
    std::string instance_name;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> connections; // pin_name → net_name
};

// 复合元件蓝图定义
struct CircuitDefinition {
    std::string name;
    std::vector<std::string> nets; // 内部线网名
    std::vector<ComponentRecipe> components; // 子元件配方
    std::unordered_map<std::string, std::string> input_expose; // pin_name → net_name
    std::unordered_map<std::string, std::string> output_expose; // pin_name → net_name
};

// 全局定义库
class CircuitLibrary {
public:
    static CircuitLibrary &instance();

    void add(CircuitDefinition def);
    const CircuitDefinition *find(const std::string &name) const;
    bool exists(const std::string &name) const;

    // JSON 支持
    std::string exportJson() const;
    void importJson(const std::string &json, std::string &error);

    const auto &definitions() const {
        return _defs;
    }

private:
    std::unordered_map<std::string, CircuitDefinition> _defs;
};

} // namespace dsc
