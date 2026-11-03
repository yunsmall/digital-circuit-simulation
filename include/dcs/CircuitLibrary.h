// CircuitLibrary — 复合元件共享蓝图注册表
//
// 多个 CompositeComponent 实例可共享同一个 CircuitDefinition，
// JSON 导出时定义只出现一次，实例引用定义名即可。
#pragma once
#include <map>
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

// 蓝图暴露的端口：net 是内部线网名，width 是位宽表达式（支持 ${参数}）。
// width 为空表示沿用旧规则（1 位，实际宽度由外部连接推断）
struct ExposedPin {
    std::string net;
    std::string width;
};

// 复合元件蓝图定义
struct CircuitDefinition {
    std::string name;
    // 这个定义从哪来：内联定义的来源标签，或蓝图文件的路径。只用于诊断输出
    std::string source;
    // 定义所在目录：展开子元件时作为 Context 的当前目录，元件自己解析相对路径
    std::string base_dir;
    // 蓝图参数默认值；实例可以用同名参数覆盖，配方/端口里的 ${name} 会被替换
    std::map<std::string, std::string> params;
    // 每次登记时库分配的递增编号。用来判断"这次导入覆盖了哪些定义"：
    // 内容相同的定义被重新导入时 source 不变，只有编号能反映覆盖
    std::size_t import_serial = 0;
    std::vector<std::string> nets; // 内部线网名
    std::vector<ComponentRecipe> components; // 子元件配方
    std::map<std::string, ExposedPin> input_expose; // pin_name → 内部端口
    std::map<std::string, ExposedPin> output_expose; // pin_name → 内部端口
};

// 替换字符串里的 ${name}。遇到未定义的参数返回 false，missing 里写参数名
bool resolveTemplate(const std::string &src, const std::map<std::string, std::string> &params, std::string &out,
                     std::string &missing);

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
    // source 只是记录在定义上，供 --list-definitions 之类的诊断用；
    // base_dir 用于展开时解析元件参数里的相对路径
    void importJson(const std::string &json, std::string &error, const std::string &source = "",
                    const std::string &base_dir = "");

    const auto &definitions() const {
        return _defs;
    }

    // 导入失败回滚：把定义表整体拷出来，失败时恢复。导入会覆盖同名定义，
    // 没有快照就没法区分"本次新增"和"调用方原有"
    std::unordered_map<std::string, CircuitDefinition> snapshot() const {
        return _defs;
    }
    void restore(std::unordered_map<std::string, CircuitDefinition> defs) {
        _defs = std::move(defs);
    }

private:
    std::unordered_map<std::string, CircuitDefinition> _defs;
    std::vector<std::string> _search_paths;
    std::size_t _import_counter = 0;
};

} // namespace dsc
