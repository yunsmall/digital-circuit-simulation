// CompositeComponent — 复合元件实现
#include "dcs/components/Composite.h"
#include <format>
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"

namespace dsc {

CompositeComponent::CompositeComponent(const std::string &name, const std::string &definition_name) :
    Component(name, "composite"), _def_name(definition_name) {
    setParam("definition", definition_name);
}

// ============================================================
// prepare — 加载定义、创建引脚
// ============================================================

bool CompositeComponent::prepare(std::string &error) {
    if (_prepared)
        return true;

    auto *def = CircuitLibrary::instance().find(_def_name);
    if (!def) {
        error = std::format("复合元件 '{}': 定义 '{}' 未在 CircuitLibrary 中注册", name(), _def_name);
        return false;
    }

    loadPinsFromDef();
    _prepared = true;
    return true;
}

void CompositeComponent::loadPinsFromDef() {
    auto *def = CircuitLibrary::instance().find(_def_name);
    if (!def)
        return;

    // 输入引脚：位宽从所连内部线网的目标输入引脚推断（默认 1）
    for (auto &[pin_name, net_name]: def->input_expose) {
        int bw = 1; // 默认位宽，后续连接时自动更新
        addInput(pin_name, bw);
    }

    // 输出引脚：位宽从所连内部线网的目标输出引脚推断（默认 1）
    for (auto &[pin_name, net_name]: def->output_expose) {
        int bw = 1;
        addOutput(pin_name, bw);
    }
}

std::string CompositeComponent::genFuncDef_seq() const {
    return std::format("// Composite: {} (definition: {})\n", name(), _def_name);
}

std::unordered_map<std::string, std::string> CompositeComponent::exportParams() const {
    return {{"definition", _def_name}};
}

std::unique_ptr<Component> CompositeComponent::clone(const std::string &n) const {
    return std::make_unique<CompositeComponent>(n, _def_name);
}

// ============================================================
// expandTo — 按定义配方展开到父电路
// ============================================================

void CompositeComponent::expandTo(Circuit &parent, Expansion &out) const {
    auto *def = CircuitLibrary::instance().find(_def_name);
    if (!def)
        return;

    std::string prefix = name() + "_";
    std::unordered_map<std::string, Net *> net_map; // 配方 net_name → parent Net*

    // ---- 1. 输出引脚：内部线网直接映射到外部线网 ----
    for (auto &[pin_name, net_name]: def->output_expose) {
        Net *ext_net = nullptr;
        for (auto &op: outputs()) {
            if (op->name() == pin_name && op->net()) {
                ext_net = op->net();
                break;
            }
        }
        if (ext_net) {
            net_map[net_name] = ext_net;
            out.output_bindings.push_back({-1, ext_net->id()}); // 标记为输出绑定
        }
    }

    // ---- 2. 输入引脚：内部线网直接映射到外部线网 ----
    for (auto &[pin_name, net_name]: def->input_expose) {
        Net *ext_net = nullptr;
        for (auto &ip: inputs()) {
            if (ip->name() == pin_name && ip->net()) {
                ext_net = ip->net();
                break;
            }
        }
        if (ext_net) {
            net_map[net_name] = ext_net;
            out.input_bindings.push_back({ext_net->id(), -1}); // 标记为输入绑定
        }
    }

    // ---- 3. 未暴露的内部线网：在 parent 中新建 ----
    for (auto &net_name: def->nets) {
        if (net_map.find(net_name) == net_map.end()) {
            auto *pnet = parent.createNet(prefix + net_name);
            net_map[net_name] = pnet;
        }
    }

    // ---- 4. 解除自身引脚绑定（展开后子元件独占驱动权）----
    for (auto &op: outputs())
        op->setNet(nullptr);
    for (auto &ip: inputs())
        ip->setNet(nullptr);

    // ---- 5. 按配方创建子元件 ----
    for (auto &recipe: def->components) {
        auto comp = ComponentFactory::instance().createUnchecked(recipe.type_name, prefix + recipe.instance_name,
                                                                 recipe.params);
        if (!comp)
            continue;

        Component *cptr = parent.addComponent(std::move(comp));

        // 重连引脚到配方指定的线网
        for (auto &[pin_name, net_name]: recipe.connections) {
            auto it = net_map.find(net_name);
            if (it != net_map.end())
                parent.connect(cptr, pin_name, it->second);
        }

        out.components.push_back(cptr);
    }
}

} // namespace dsc
