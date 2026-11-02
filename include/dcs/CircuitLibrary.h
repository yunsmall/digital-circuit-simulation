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
    // 这个定义从哪来：内联定义的来源标签，或蓝图文件的路径。只用于诊断输出
    std::string source;
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

    // 蓝图库查找路径（类似编译器的 -I）：definitions 里的相对路径先按引用它的
    // JSON 所在目录找，找不到再按这里的顺序逐个找。CLI 的 --lib-path 就是往这里加
    void addSearchPath(const std::string &dir);
    const std::vector<std::string> &searchPaths() const {
        return _search_paths;
    }
    void clearSearchPaths() {
        _search_paths.clear();
    }

    // JSON 支持
    std::string exportJson() const;
    // source 只是记录在定义上，供 --list-definitions 之类的诊断用
    void importJson(const std::string &json, std::string &error, const std::string &source = "");

    const auto &definitions() const {
        return _defs;
    }

private:
    std::unordered_map<std::string, CircuitDefinition> _defs;
    std::vector<std::string> _search_paths;
};

} // namespace dsc
