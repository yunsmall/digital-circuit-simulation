// CompositeComponent — 复合元件（引用共享蓝图）
//
// 多个 CompositeComponent 实例可共享同一个 CircuitDefinition，
// 定义存于 CircuitLibrary 中，展开时按配方创建子元件。
//
// 用法:
//   1. 先注册定义到 CircuitLibrary
//   2. 创建实例: auto c = std::make_unique<CompositeComponent>("ha1", "half_adder");
//   3. 或通过 JSON: 顶层 definitions 中放蓝图，实例用 "definition" 字段引用
#pragma once
#include <functional>
#include <map>
#include <unordered_map>
#include "dcs/Circuit.h"
#include "dcs/Component.h"

namespace dsc {

struct CircuitDefinition;
class CircuitLibrary;

class CompositeComponent : public Component {
public:
    // definition_name: CircuitLibrary 中注册的定义名
    // params: 实例覆盖的蓝图参数（未给的用定义里的默认值）
    CompositeComponent(const std::string &name, const std::string &definition_name,
                       std::map<std::string, std::string> params = {});

    // 定义来自哪个库。Circuit::addComponent 会设置成它所在上下文的库；
    // 不设置时退回进程默认库（兼容直接 new 出来用的老代码）
    void setLibrary(const CircuitLibrary *lib) {
        _lib = lib;
    }

    // 引用的定义名
    const std::string &definitionName() const {
        return _def_name;
    }

    bool hasCombinational() const override {
        return false;
    }
    bool hasSequential() const override {
        return true;
    }
    bool isComposite() const override {
        return true;
    }
    std::string genFuncDef_seq() const override;
    std::unique_ptr<Component> clone(const std::string &n) const override;

    // 编译前准备：加载定义、创建引脚
    bool prepare(Context &ctx, std::string &error) override;

    // 导出构造参数
    std::unordered_map<std::string, std::string> exportParams() const override;

    // 展开：将子电路的元件和线网注入到父电路中
    struct Expansion {
        std::vector<Component *> components;
        std::vector<std::pair<int, int>> input_bindings;
        std::vector<std::pair<int, int>> output_bindings;
        // 展开过程中任一子元件创建失败或引脚连接失败时写入原因；
        // 非空表示这次展开不完整，Circuit::compile() 直接报错而不是带着残缺电路继续
        std::string error;
    };
    void expandTo(Circuit &parent, Expansion &out) const;

private:
    std::string _def_name;
    // 实例覆盖的蓝图参数；默认值在定义里
    std::map<std::string, std::string> _params;
    bool _prepared = false;
    const CircuitLibrary *_lib = nullptr;

    const CircuitLibrary &lib() const;

    // 从定义加载引脚（width 表达式按有效参数解析）
    bool loadPinsFromDef(const std::map<std::string, std::string> &params, std::string &error);
    std::map<std::string, std::string> effectiveParams(const CircuitDefinition &def) const;
};

} // namespace dsc
