// CompositeComponent — 复合元件实现
#include "dcs/components/platform/Composite.h"
#include <charconv>
#include <format>
#include "dcs/CircuitLibrary.h"
#include "dcs/util/TemplateExpr.h"
#include "dcs/ComponentFactory.h"
#include "dcs/Context.h"

namespace dsc {

CompositeComponent::CompositeComponent(const std::string &name, const std::string &definition_name,
                                       std::map<std::string, std::string> params) :
    Component(name, "composite"), _def_name(definition_name), _params(std::move(params)) {
    setParam("definition", definition_name);
    for (auto &[k, v]: _params)
        setParam(k, v);
}

// ============================================================
// prepare — 加载定义、创建引脚
// ============================================================

std::map<std::string, std::string> CompositeComponent::effectiveParams(const CircuitDefinition &def) const {
    std::map<std::string, std::string> p = def.params;
    for (auto &[k, v]: _params)
        p[k] = v; // 实例覆盖定义默认值
    return p;
}

bool CompositeComponent::ensurePins(Context &ctx, std::string &error) {
    if (_pins_built)
        return true;

    // 定义必须来自实例所在上下文的库，和 expandTo 用 parent.context().library()
    // 保持同一个来源；不同 Context 可以有同名但内容不同的蓝图
    auto *def = ctx.library().find(_def_name);
    if (!def) {
        error = std::format("复合元件 '{}': 定义 '{}' 未在 CircuitLibrary 中注册", name(), _def_name);
        return false;
    }
    // 拼错的参数不能静默忽略：展开结果会少一层配置，运行期才暴露
    for (auto &[k, v]: _params) {
        if (def->params.find(k) == def->params.end()) {
            error = std::format("复合元件 '{}': 定义 '{}' 没有参数 '{}'", name(), _def_name, k);
            return false;
        }
    }

    if (!loadPinsFromDef(*def, effectiveParams(*def), error))
        return false;
    _pins_built = true;
    return true;
}

bool CompositeComponent::loadPinsFromDef(const CircuitDefinition &def,
                                         const std::map<std::string, std::string> &params, std::string &error) {
    auto pin_width = [&](const ExposedPin &ep, const char *pin_name, const char *dir) {
        if (ep.width.empty())
            return 1;
        std::string text;
        TemplateExpr expr(ep.width, params);
        if (!expr.resolve(text)) {
            if (!expr.missingParam().empty())
                error = std::format("复合元件 '{}': 端口 '{}' 的位宽引用了未定义参数 '{}'", name(), pin_name,
                                    expr.missingParam());
            else
                error = std::format("复合元件 '{}': 端口 '{}' 的位宽表达式错误: {}", name(), pin_name, expr.error());
            return 0;
        }
        int w = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), w);
        if (ec != std::errc{} || ptr != text.data() + text.size() || w < 1 || w > 128) {
            error = std::format("复合元件 '{}': {} 端口 '{}' 的位宽 '{}' 无效（1..128）", name(), dir, pin_name, text);
            return 0;
        }
        return w;
    };

    // 先把所有位宽算出来再建引脚：中途失败不能留下半套引脚
    std::vector<std::pair<std::string, int>> ins, outs;
    for (auto &[pin_name, ep]: def.input_expose) {
        const int bw = pin_width(ep, pin_name.c_str(), "输入");
        if (bw == 0)
            return false;
        ins.emplace_back(pin_name, bw);
    }
    for (auto &[pin_name, ep]: def.output_expose) {
        const int bw = pin_width(ep, pin_name.c_str(), "输出");
        if (bw == 0)
            return false;
        outs.emplace_back(pin_name, bw);
    }
    for (auto &[pin_name, bw]: ins)
        addInput(pin_name, bw);
    for (auto &[pin_name, bw]: outs)
        addOutput(pin_name, bw);
    return true;
}

std::string CompositeComponent::genFuncDef_seq() const {
    return std::format("// Composite: {} (definition: {})\n", cgen::commentText(name()),
                       cgen::commentText(_def_name));
}

std::unordered_map<std::string, std::string> CompositeComponent::exportParams() const {
    std::unordered_map<std::string, std::string> p{{"definition", _def_name}};
    for (auto &[k, v]: _params)
        p[k] = v;
    return p;
}

std::unique_ptr<Component> CompositeComponent::clone(const std::string &n) const {
    return std::make_unique<CompositeComponent>(n, _def_name, _params);
}

// ============================================================
// expandTo — 按定义配方展开到父电路
// ============================================================

void CompositeComponent::expandTo(Circuit &parent, Expansion &out) const {
    // 以父电路的上下文为准：同一个蓝图名在不同 Context 里可以是不同定义
    auto *def = parent.context().library().find(_def_name);
    if (!def) {
        // ensurePins 时检查过定义存在，但 expandTo 是独立入口，Context 换过之后
        // 定义可能已经找不到了。静默返回会让调用方把整个复合元件当成展开成功删掉
        out.error = std::format("复合元件 '{}': 定义 '{}' 不存在", name(), _def_name);
        return;
    }

    const auto params = effectiveParams(*def);
    // 所有配方/线网里的 ${name} 在展开时一次性替换；缺参数直接报错，
    // 不能带着字面量 "${x}" 去创建元件/连线
    auto R = [&](const std::string &src, std::string &dst) {
        TemplateExpr expr(src, params);
        if (expr.resolve(dst))
            return true;
        out.error = expr.missingParam().empty()
                            ? std::format("复合元件 '{}': 展开时模板错误: {}", name(), expr.error())
                            : std::format("复合元件 '{}': 展开时引用了未定义参数 '{}'", name(), expr.missingParam());
        return false;
    };

    std::string prefix = name() + "_";
    std::unordered_map<std::string, Net *> net_map; // 配方 net_name → parent Net*

    // 两个 expose 的 net 字面量不同、模板求值后却可能撞成同一个名字
    // （例如 "n1" 和 "n${x}"）；静默覆盖会让其中一个引脚凭空悬空
    auto bind_exposed = [&](const std::string &key, Net *ext, const std::string &pin_name) {
        auto [it, inserted] = net_map.try_emplace(key, ext);
        if (!inserted && it->second != ext) {
            out.error = std::format("复合元件 '{}' 展开失败：引脚 '{}' 的内部线网 '{}' 与已有绑定冲突", name(),
                                    pin_name, key);
            return false;
        }
        return true;
    };

    // ---- 1. 输出引脚：内部线网直接映射到外部线网 ----
    for (auto &[pin_name, ep]: def->output_expose) {
        Net *ext_net = nullptr;
        for (auto &op: outputs()) {
            if (op->name() == pin_name && op->net()) {
                ext_net = op->net();
                break;
            }
        }
        if (ext_net) {
            std::string net_name;
            if (!R(ep.net, net_name))
                return;
            if (!bind_exposed(net_name, ext_net, pin_name))
                return;
            out.output_bindings.push_back({-1, ext_net->id()}); // 标记为输出绑定
        }
    }

    // ---- 2. 输入引脚：内部线网直接映射到外部线网 ----
    for (auto &[pin_name, ep]: def->input_expose) {
        Net *ext_net = nullptr;
        for (auto &ip: inputs()) {
            if (ip->name() == pin_name && ip->net()) {
                ext_net = ip->net();
                break;
            }
        }
        if (ext_net) {
            std::string net_name;
            if (!R(ep.net, net_name))
                return;
            if (!bind_exposed(net_name, ext_net, pin_name))
                return;
            out.input_bindings.push_back({ext_net->id(), -1}); // 标记为输入绑定
        }
    }

    // ---- 3. 未暴露的内部线网：在 parent 中新建 ----
    for (auto &net_name: def->nets) {
        std::string resolved;
        if (!R(net_name, resolved))
            return;
        if (net_map.find(resolved) == net_map.end()) {
            auto *pnet = parent.createNet(prefix + resolved);
            net_map[resolved] = pnet;
        }
    }

    // ---- 4. 解除自身引脚绑定（展开后子元件独占驱动权）----
    for (auto &op: outputs())
        op->setNet(nullptr);
    for (auto &ip: inputs())
        ip->setNet(nullptr);

    // ---- 5. 按配方创建子元件 ----
    // 子元件里的相对路径按定义来源目录解析（内联定义就是引用它的 JSON 目录，
    // 文件蓝图就是该文件所在目录），不能沿用父电路当前目录
    Context::ScopedSourceDir source_guard(parent.context(), def->base_dir);
    for (auto &recipe: def->components) {
        std::string inst_name;
        if (!R(recipe.instance_name, inst_name))
            return;

        std::unordered_map<std::string, std::string> recipe_params;
        for (auto &[k, v]: recipe.params) {
            std::string resolved;
            if (!R(v, resolved))
                return;
            recipe_params[k] = resolved;
        }

        std::string create_err;
        // 配方参数来自蓝图 JSON（用户可控），必须走带 valid_values 校验的入口；
        // 内部 API 无校验版本会让非法值拖到生成 C 代码时才抛异常
        auto comp = ComponentFactory::instance().create(recipe.type_name, prefix + inst_name, recipe_params,
                                                        create_err);
        if (!comp) {
            // 类型名写错或参数非法时静默跳过，电路会少一个子元件，运行期表现为
            // "莫名其妙不工作"，而且编译期没有任何提示。必须当成展开失败报出来
            out.error = std::format("复合元件展开失败：{}", create_err);
            return;
        }

        Component *cptr = parent.addComponent(std::move(comp));
        if (!cptr) {
            // addComponent 回滚了这次加入（prepare 失败），展开不完整，
            // 不能继续拿空指针去 connect
            out.error = std::format("复合元件展开失败：子元件 '{}' 准备失败", inst_name);
            return;
        }

        // 重连引脚到配方指定的线网
        for (auto &[pin_name, net_name]: recipe.connections) {
            std::string resolved;
            if (!R(net_name, resolved))
                return;
            auto it = net_map.find(resolved);
            if (it == net_map.end()) {
                // 线网名拼错时静默跳过会让子元件引脚悬空，运行期只表现为
                // "莫名其妙不工作"；和引脚名写错一样必须当展开失败报出来
                out.error = std::format(
                        "复合元件 '{}' 展开失败：元件 '{}' 的引脚 '{}' 连接的线网 '{}' "
                        "既不在定义的 nets 里，也不在任何 expose 引脚上",
                        name(), recipe.instance_name, pin_name, resolved);
                return;
            }
            auto err = parent.connect(cptr, pin_name, it->second);
            if (err.code != ErrorCode::OK) {
                // 配方写错引脚名时不能静默悬空——展开后的电路和配方不符，
                // 运行期只会表现为"莫名其妙不工作"
                out.error = std::format("复合元件展开失败：{}", err.message);
                return;
            }
        }

        out.components.push_back(cptr);
    }
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"composite", {{"definition", ""}}, {}, {}, dsc::ComponentCategory::Misc,
     "复合元件，由 JSON 蓝图定义嵌套子电路；蓝图参数按 ${name} 字符串替换", true},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            std::map<std::string, std::string> params;
            for (auto &[k, v]: p)
                if (k != "definition")
                    params[k] = v;
            return std::make_unique<dsc::CompositeComponent>(n, dsc::paramStr(p, "definition", ""), std::move(params));
        });
} // namespace
