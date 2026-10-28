// NetlistGraph - 网表的构建与校验
//
// 这里放的是建图相关的成员：连线、断开、删除、环路检测、复合元件展开。
// 它们直接读写 Circuit 的 _nets/_components/_bus_nets，所以仍实现为 Circuit 的成员；
// 单独成文件是为了让网表结构和仿真运行分开看。
// 这些方法都不在 tick 热路径上，拆文件不带来任何运行期开销。
#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/Composite.h"

#include <algorithm>
#include <format>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace dsc {

void Circuit::connect(Component *comp, const std::string &pin_name, Net *net) {
    Pin *pin = findPin(comp, pin_name);
    if (!pin)
        throw std::runtime_error(std::format("元件 '{}' 没有引脚 '{}'", comp->name(), pin_name));

    if (pin->isOutput()) {
        // 找到此输出在元件中的索引
        int this_out_idx = -1;
        for (int i = 0; i < (int) comp->outputs().size(); i++) {
            if (comp->outputs()[i].get() == pin) {
                this_out_idx = i;
                break;
            }
        }

        for (auto &c: _components) {
            for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
                auto &p = c->outputs()[oi];
                if (p->net() == net && p.get() != pin) {
                    // 双方都是三态输出 → 允许多驱动，标记总线
                    if (pin->isTriState() && p->isTriState()) {
                        auto &drivers = _bus_nets[net->id()];
                        auto add_driver = [&](Component *drv, int oidx) {
                            for (auto &d : drivers)
                                if (d.comp == drv && d.out_idx == oidx)
                                    return;
                            drivers.push_back({drv, oidx});
                        };
                        add_driver(c.get(), oi);
                        add_driver(comp, this_out_idx);
                        goto allow_connect;
                    }
                    throw std::runtime_error(std::format("线网 '{}' 已被多个输出驱动", net->name()));
                }
            }
        }
    }
allow_connect:
    pin->setNet(net);
    net->updateBitWidth(pin->bit_width());
    _compiled = false; // 连接关系变了，生成代码里的读写顺序需要重新生成
}

void Circuit::disconnect(Component *comp, const std::string &pin_name) {
    Pin *pin = findPin(comp, pin_name);
    if (!pin || !pin->net())
        return; // 未连接，无需操作

    int net_id = pin->net()->id();

    // 输出引脚：从 _bus_nets 中移除对应的三态驱动记录
    if (pin->isOutput()) {
        auto it = _bus_nets.find(net_id);
        if (it != _bus_nets.end()) {
            // 找到该引脚在元件输出中的索引
            int out_idx = -1;
            for (int i = 0; i < (int) comp->outputs().size(); i++) {
                if (comp->outputs()[i].get() == pin) {
                    out_idx = i;
                    break;
                }
            }
            auto &drivers = it->second;
            drivers.erase(std::remove_if(drivers.begin(), drivers.end(),
                                         [comp, out_idx](const BusDriver &d) {
                                             return d.comp == comp && d.out_idx == out_idx;
                                         }),
                          drivers.end());
            if (drivers.empty())
                _bus_nets.erase(it);
        }
    }

    pin->setNet(nullptr);
    _compiled = false;
}

void Circuit::disconnectAll(Component *comp) {
    // 先断输出（清理 _bus_nets），再断输入
    for (auto &p : comp->outputs())
        disconnect(comp, p->name());
    for (auto &p : comp->inputs())
        disconnect(comp, p->name());
}

void Circuit::removeComponent(Component *comp) {
    disconnectAll(comp);
    auto it = std::find_if(_components.begin(), _components.end(),
                           [comp](const auto &uptr) { return uptr.get() == comp; });
    if (it != _components.end())
        _components.erase(it);
    _compiled = false;
}

void Circuit::removeNet(Net *net) {
    // 断开所有连接到此线网的引脚
    for (auto &c : _components) {
        for (auto &p : c->outputs()) {
            if (p->net() == net)
                p->setNet(nullptr);
        }
        for (auto &p : c->inputs()) {
            if (p->net() == net)
                p->setNet(nullptr);
        }
    }
    // 清理 _bus_nets
    _bus_nets.erase(net->id());

    // 从 _nets 中移除并销毁
    auto it = std::find_if(_nets.begin(), _nets.end(),
                           [net](const auto &uptr) { return uptr.get() == net; });
    if (it != _nets.end())
        _nets.erase(it);

    // 重编号：_w[] 的下标就是 net id，删掉中间一个后必须补平，否则后续生成的代码会越界
    std::vector<int> old_ids(_nets.size());
    for (size_t i = 0; i < _nets.size(); ++i)
        old_ids[i] = _nets[i]->id();
    std::map<int, std::vector<BusDriver>> old_bus;
    old_bus.swap(_bus_nets);
    for (size_t i = 0; i < _nets.size(); ++i) {
        _nets[i]->setId((int) i);
        auto bit = old_bus.find(old_ids[i]);
        if (bit != old_bus.end())
            _bus_nets[(int) i] = std::move(bit->second);
    }

    _compiled = false;
}

void Circuit::removeAllNets() {
    // 断开所有元件的全部引脚
    for (auto &c : _components)
        disconnectAll(c.get());
    _nets.clear();
    _bus_nets.clear();
    _compiled = false;
}

Net *Circuit::findNet(const std::string &name) const {
    for (auto &n: _nets)
        if (n->name() == name)
            return n.get();
    return nullptr;
}

Component *Circuit::componentById(int id) const {
    if (id < 0 || id >= (int) _components.size())
        return nullptr;
    return _components[id].get();
}

Pin *Circuit::findPin(Component *comp, const std::string &name) {
    for (auto &p: comp->inputs())
        if (p->name() == name)
            return p.get();
    for (auto &p: comp->outputs())
        if (p->name() == name)
            return p.get();
    return nullptr;
}

// ============================================================
// 环路检测
// ============================================================

CircuitError Circuit::check() {
    int N = (int) _nets.size();
    std::vector<std::vector<int>> adj(N);
    for (auto &comp: _components) {
        for (int oi = 0; oi < (int) comp->outputs().size(); oi++) {
            Pin *out_pin = comp->outputs()[oi].get();
            Net *out_net = out_pin->net();
            if (!out_net)
                continue;
            for (int di: comp->getCombinationalDeps(oi)) {
                Net *in_net = comp->inputs()[di]->net();
                if (!in_net)
                    continue;
                adj[in_net->id()].push_back(out_net->id());
            }
        }
    }

    std::vector<Color> color(N, Color::WHITE);
    std::vector<int> path;
    for (int i = 0; i < N; i++) {
        if (color[i] == Color::WHITE) {
            if (dfsCycle(i, adj, color, path)) {
                std::ostringstream oss;
                oss << "检测到组合逻辑环路: ";
                bool in_cycle = false;
                int cycle_start = path.back();
                for (int nid: path) {
                    if (nid == cycle_start)
                        in_cycle = true;
                    if (in_cycle)
                        oss << _nets[nid]->name() << " -> ";
                }
                oss << _nets[cycle_start]->name();
                return {ErrorCode::COMBINATIONAL_LOOP, -1, {}, oss.str()};
            }
        }
    }
    return {ErrorCode::OK};
}

bool Circuit::dfsCycle(int u, const std::vector<std::vector<int>> &adj, std::vector<Color> &color,
                       std::vector<int> &path) {
    color[u] = Color::GRAY;
    path.push_back(u);
    for (int v: adj[u]) {
        if (color[v] == Color::GRAY) {
            path.push_back(v);
            return true;
        }
        if (color[v] == Color::WHITE && dfsCycle(v, adj, color, path))
            return true;
    }
    color[u] = Color::BLACK;
    path.pop_back();
    return false;
}

// ============================================================
// 复合元件展开
// ============================================================

void Circuit::expandComposites() {
    bool found = true;
    while (found) {
        found = false;
        std::vector<CompositeComponent *> comps;
        for (auto &c: _components)
            if (c->isComposite())
                comps.push_back(static_cast<CompositeComponent *>(c.get()));

        if (comps.empty())
            break;

        for (auto *comp: comps) {
            CompositeComponent::Expansion exp;
            comp->expandTo(*this, exp);
            found = true;
            auto it = std::find_if(_components.begin(), _components.end(), [comp](auto &c) { return c.get() == comp; });
            if (it != _components.end()) {
                _components.erase(it);
            }
        }
    }
}

} // namespace dsc
