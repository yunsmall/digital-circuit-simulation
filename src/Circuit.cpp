#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/Composite.h"
#include "dcs/util/ExePath.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// JIT 头文件搜索路径（静态成员）
std::string dsc::Circuit::_jit_include_dir;

void dsc::Circuit::setJitIncludeDir(const std::string &dir) {
    _jit_include_dir = dir;
}

const std::string &dsc::Circuit::jitIncludeDir() {
    return _jit_include_dir;
}

#ifdef DCS_USE_TCC
// ---- TinyCC 后端 ----
#include <libtcc.h>
#else
// ---- LLVM/Clang 后端 ----
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/PreprocessorOptions.h>
#endif

// 总线冲突回调（JIT 注入，通过 _circuit_ptr 回调到 Circuit 对象）
extern "C" void dcs_bus_conflict(void *circuit, int net_id) {
    static_cast<dsc::Circuit *>(circuit)->onBusConflict(net_id);
}

namespace dsc {
void dcsBusConflictThunk(void *circuit_ptr, int net_id) {
    dcs_bus_conflict(circuit_ptr, net_id);
}
} // namespace dsc

namespace dsc {

// 编译器环境初始化（内部调用，只执行一次）
static void init_compiler_env() {
#ifndef DCS_USE_TCC
    static bool done = false;
    if (done)
        return;
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();
    done = true;
#endif
}

// ============================================================
// Circuit 构造/析构
// ============================================================

Circuit::Circuit() = default;

Circuit::~Circuit() {
    clear();
}

void Circuit::deinit() {
    // 销毁元件状态（JIT circuit_deinit 中调用 dcs_dll_destroy）
    if (_deinit_fn)
        _deinit_fn();

    // 仿真结束通知（DLL: dcs_dll_deinit）
    for (auto &comp: _components)
        comp->simDeinit();
}

void Circuit::clear() {
    jitCleanup();
    _nets.clear();
    _components.clear();
    _bus_nets.clear();
}

void Circuit::reset() {
    if (_reset_fn)
        _reset_fn();
}

void Circuit::jitCleanup() {
    // 仅清理 JIT 状态（函数指针 + 后端资源）
    _tick_fn = nullptr;
    _init_fn = nullptr;
    _deinit_fn = nullptr;
    _reset_fn = nullptr;
    _set_wire_fn = nullptr;
    _get_wire_fn = nullptr;
    _backend.cleanup();
    _compiled = false;
}

// ============================================================
// 构建电路
// ============================================================

Net *Circuit::createNet(const std::string &name) {
    int id = (int) _nets.size();
    auto net = std::make_unique<Net>(name, id);
    Net *ptr = net.get();
    _nets.push_back(std::move(net));
    return ptr;
}

Component *Circuit::addComponent(std::unique_ptr<Component> comp) {
    // 加入电路时立即准备元件（DLL 加载、读取描述符创建引脚等）
    std::string prep_err;
    if (!comp->prepare(prep_err))
        throw std::runtime_error(prep_err);
    comp->setId((int) _components.size());
    Component *ptr = comp.get();
    _components.push_back(std::move(comp));
    _compiled = false; // 元件集合变了，旧的 JIT 代码不再对应当前电路
    return ptr;
}

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

// ============================================================
// C 代码生成
// ============================================================

std::string Circuit::generateC() const {
    std::ostringstream oss;
    oss << R"(#include <dcs_stdint.h>
#include <dcs_stdbool.h>
#include <dcs_util.h>

)";
    oss << std::format("#define NET_COUNT {}\n", _nets.size());
    oss << std::format("#define COMP_COUNT {}\n\n", _components.size());
    oss << "static uint8_t _w[NET_COUNT][16];\n";
    oss << std::format("static void* _circuit_ptr = (void*)0x{:X};\n", reinterpret_cast<uintptr_t>(this));
    oss << "extern void dcs_bus_conflict(void*, int);\n\n";

    // 每个元件的每个输出：_c_out_<comp_id>_<out_idx>[16] + _c_oe_<comp_id>_<out_idx>
    for (auto &c: _components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("static uint8_t _c_out_{}_{}[16];\n", c->id(), oi);
            bool ts = c->outputs()[oi]->isTriState();
            oss << std::format("static bool _c_oe_{}_{} = {};\n", c->id(), oi, ts ? "false" : "true");
        }
    }
    oss << "\n";

    std::vector<Component *> comb, seq;
    for (auto &c: _components) {
        if (c->hasCombinational())
            comb.push_back(c.get());
        if (c->hasSequential())
            seq.push_back(c.get());
    }

    for (auto &c: _components) {
        auto def = c->genStructDef();
        if (!def.empty())
            oss << def << "\n\n";
    }
    for (auto &c: _components) {
        auto decl = c->genStateDecl();
        if (!decl.empty())
            oss << decl << "\n";
    }
    oss << "\n";

    auto sorted = topologicalSort(comb);

    // === 前向声明 ===
    oss << "// ---- 组合逻辑 ----\n";
    for (auto *c: sorted)
        if (c->hasCombinational())
            oss << c->genFuncDecl_comb() << "\n";
    oss << "// ---- 时序逻辑 ----\n";
    for (auto &c: _components) {
        auto *s = c.get();
        if (s->hasSequential())
            oss << s->genFuncDecl_seq() << "\n";
    }
    oss << "\n";

    // === 函数定义 ===
    oss << "// ---- 组合逻辑 ----\n";
    for (auto *c: sorted)
        if (c->hasCombinational())
            oss << c->genFuncDef_comb() << "\n";
    oss << "// ---- 时序逻辑 ----\n";
    for (auto &c: _components) {
        auto *s = c.get();
        if (s->hasSequential())
            oss << s->genFuncDef_seq() << "\n";
    }

    // === circuit_tick ===
    oss << "void circuit_tick(void) {\n";
    // 先算出哪些组合元件要重跑：输入直接或间接挂在总线网上的那些。
    // 没有总线网的电路集合为空，阶段2b 就是零开销。
    std::set<Component *> bus_affected;
    {
        std::vector<Component *> queue;
        for (auto *c: sorted) {
            if (!c->hasCombinational())
                continue;
            for (auto &pin: c->inputs())
                if (pin->net() && _bus_nets.count(pin->net()->id())) {
                    if (bus_affected.insert(c).second)
                        queue.push_back(c);
                    break;
                }
        }
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            for (auto &op: queue[qi]->outputs()) {
                if (!op->net())
                    continue;
                for (auto *down: sorted) {
                    if (!down->hasCombinational() || bus_affected.count(down))
                        continue;
                    for (auto &pin: down->inputs())
                        if (pin->net() == op->net()) {
                            bus_affected.insert(down);
                            queue.push_back(down);
                            break;
                        }
                }
            }
        }
    }

    // 组合阶段生成器：执行所有组合元件，并把非总线输出立即传播到 _w
    auto emitCombStage = [&](const std::set<Component *> *only) {
        for (auto *c: sorted) {
            if (only && !only->count(c))
                continue;
            if (!c->hasCombinational())
                continue;
            oss << "    " << c->funcName_comb() << "();\n";
            for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
                Net *n = c->outputs()[oi]->net();
                if (!n)
                    continue;
                int nid = n->id();
                if (_bus_nets.count(nid))
                    continue; // 总线网留给决议阶段
                int nb = byte_count(n->bit_width());
                oss << std::format("    if (_c_oe_{}_{}) {{\n", c->id(), oi);
                oss << std::format("        dcs_memcpy(_w[{}], _c_out_{}_{}, {});\n", nid, c->id(), oi, nb);
                oss << std::format("        dcs_memset(_w[{}] + {}, 0, {});\n", nid, nb, 16 - nb);
                oss << "    }\n";
            }
        }
    };

    // 传播阶段生成器：总线决议 + 所有输出的 _c_out → _w
    // track_change 为真时，每写一条总线都比对旧值，把"有没有变化"记到 _bus_changed，
    // 阶段 2b 靠它判断级联是否已经到达不动点
    auto emitPropagation = [&](int round, bool track_change) {
    std::set<int> bus_nets_handled;

    // 总线决议
    for (auto &[net_id, drivers]: _bus_nets) {
        int nb = byte_count(_nets[net_id]->bit_width());
        oss << "\n    // 总线 " << net_id << "（" << drivers.size() << " 个三态驱动）\n";
        oss << "    {\n";
        // 旧值必须在冲突检测之前取：冲突时下面的 goto 会跳过初���化，
        // 让它之后的比对读到未初始化的栈内存
        if (track_change)
            oss << std::format("        uint8_t _old_{}[16]; dcs_memcpy(_old_{}, _w[{}], 16);\n", net_id, net_id, net_id);
        oss << "        int _active = -1, _conflict = 0;\n";
        for (int di = 0; di < (int) drivers.size(); di++) {
            int cid = drivers[di].comp->id();
            int oi = drivers[di].out_idx;
            if (di == 0)
                oss << std::format("        if (_c_oe_{}_{}) _active = {};\n", cid, oi, di);
            else
                // 多驱动冲突只置标志，不跳转：跳转会绕过下面所有语句（包括旧值快照的初始化）
                oss << std::format(
                        "        if (_c_oe_{}_{}) {{\n"
                        "            if (_active >= 0) {{ dcs_bus_conflict(_circuit_ptr, {}); _conflict = 1; }}\n"
                        "            else _active = {};\n"
                        "        }}\n",
                        cid, oi, net_id, di);
        }
        oss << "        if (!_conflict) {\n";
        oss << "        if (_active >= 0) {\n";
        for (int di = 0; di < (int) drivers.size(); di++) {
            int cid = drivers[di].comp->id();
            int oi = drivers[di].out_idx;
            oss << std::format("            {}if (_active == {}) dcs_memcpy(_w[{}], _c_out_{}_{}, {});\n",
                               di == 0 ? "" : "else ", di, net_id, cid, oi, nb);
        }
        oss << std::format("            dcs_memset(_w[{}] + {}, 0, {});\n", net_id, nb, 16 - nb);
        oss << "        } else {\n";
        oss << std::format("            dcs_memset(_w[{}], 0, 16);  // Hi-Z\n", net_id);
        oss << "        }\n";
        oss << "        }\n";
        if (track_change)
            oss << std::format(
                    "    for (int _i=0;_i<16;_i++) if (_old_{}[_i] != _w[{}][_i]) {{ _bus_changed=1; break; }}\n",
                    net_id, net_id);
        oss << "    }\n";
        bus_nets_handled.insert(net_id);
    }

    // 非总线线网：若驱动活跃则拷贝 _c_out → _w
    for (auto &c: _components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            Net *n = c->outputs()[oi]->net();
            if (!n)
                continue;
            int nid = n->id();
            if (bus_nets_handled.count(nid))
                continue; // 总线已处理
            int nb = byte_count(n->bit_width());
            oss << std::format("    if (_c_oe_{}_{}) {{\n", c->id(), oi);
            oss << std::format("        dcs_memcpy(_w[{}], _c_out_{}_{}, {});\n", nid, c->id(), oi, nb);
            oss << std::format("        dcs_memset(_w[{}] + {}, 0, {});\n", nid, nb, 16 - nb);
            oss << "    } else {\n";
            oss << std::format("        dcs_memset(_w[{}], 0, 16);  // Hi-Z\n", nid);
            oss << "    }\n";
        }
    }
    }; // emitPropagation

    oss << "    // 阶段1: 组合逻辑\n";
    emitCombStage(nullptr);
    if (!bus_affected.empty()) oss << "    int _bus_changed = 0;\n";
    oss << "    // 阶段2: 总线决议 + 时序输出传播\n";
    emitPropagation(0, false);
    // 读总线的组合元件在阶段1 看到的是决议前的值，必须补跑。补跑会改动总线，
    // 又可能影响下一级读总线的元件，所以不能只补一轮——一直迭代到总线不再变化。
    // 上限取元件数，防止电路里出现自激振荡时死循环。
    if (!bus_affected.empty()) {
        oss << "    // 阶段2b: 反复重跑受总线影响的组合元件，直到总线稳定\n";
        oss << "    bool _bus_settled = false;\n";
        oss << std::format("    for (int _bus_round = 0; _bus_round < {}; _bus_round++) {{\n",
                           (int) _components.size() + 1);
        oss << "        _bus_changed = 0;\n";
        emitCombStage(&bus_affected);
        emitPropagation(2, true);
        oss << "        if (!_bus_changed) { _bus_settled = true; break; }\n";
        oss << "    }\n";
        oss << "    if (!_bus_settled) dcs_bus_conflict(_circuit_ptr, -1);\n";
    }

    oss << "    // 阶段3: 时序逻辑\n";
    for (auto &c: _components) {
        auto *s = c.get();
        if (s->hasSequential())
            oss << "    " << s->funcName_seq() << "();\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_init(void) {
    dcs_memset(_w, 0, sizeof(_w));
)";
    // 清零所有 _c_out 和 _c_oe
    for (auto &c: _components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("    dcs_memset(_c_out_{}_{}, 0, 16);\n", c->id(), oi);
        }
    }
    for (auto &c: _components) {
        auto init = c->genInitCode();
        if (!init.empty())
            oss << init << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_deinit(void) {
)";
    for (auto &c: _components) {
        auto deinit = c->genDeinitCode();
        if (!deinit.empty())
            oss << deinit << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_reset(void) {
    dcs_memset(_w, 0, sizeof(_w));
)";
    // 清零所有 _c_out 和 _c_oe
    for (auto &c: _components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("    dcs_memset(_c_out_{}_{}, 0, 16);\n", c->id(), oi);
        }
    }
    for (auto &c: _components) {
        auto reset = c->genResetCode();
        if (!reset.empty())
            oss << reset << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_set_wire(int id, const uint8_t val[16]) {
    dcs_memcpy(_w[id], val, 16);
}
void circuit_get_wire(int id, uint8_t val[16]) {
    dcs_memcpy(val, _w[id], 16);
}
)";
    return oss.str();
}

// ============================================================
// 拓扑排序
// ============================================================

std::vector<Component *> Circuit::topologicalSort(const std::vector<Component *> &comb) const {
    int C = (int) comb.size();
    std::unordered_map<Component *, int> idx;
    for (int i = 0; i < C; i++)
        idx[comb[i]] = i;

    std::vector<std::vector<int>> adj(C);
    std::vector<int> indeg(C, 0);
    std::unordered_map<int, Component *> net_writer;
    for (auto *c: comb)
        for (auto &p: c->outputs())
            if (p->net())
                net_writer[p->net()->id()] = c;

    for (int bi = 0; bi < C; bi++) {
        auto *b = comb[bi];
        for (auto &pin: b->inputs()) {
            Net *net = pin->net();
            if (!net)
                continue;
            auto it = net_writer.find(net->id());
            if (it != net_writer.end()) {
                int ai = idx[it->second];
                if (ai != bi) {
                    adj[ai].push_back(bi);
                    indeg[bi]++;
                }
            }
        }
    }

    std::vector<Component *> result;
    std::vector<int> q;
    for (int i = 0; i < C; i++)
        if (indeg[i] == 0)
            q.push_back(i);
    while (!q.empty()) {
        int u = q.back();
        q.pop_back();
        result.push_back(comb[u]);
        for (int v: adj[u])
            if (--indeg[v] == 0)
                q.push_back(v);
    }
    for (int i = 0; i < C; i++)
        if (indeg[i] > 0)
            result.push_back(comb[i]);
    return result;
}

// ============================================================
// JIT 编译
// ============================================================

CircuitError Circuit::compile() {
    // 先把入口指针全部置空再清理后端：cleanup() 会释放已编译的代码，
    // 若本次编译失败，残留的旧函数指针就成了悬空指针
    jitCleanup();

    // 展开复合元件
    expandComposites();

    // 展开后重新编号，保证 ID = 索引
    for (int i = 0; i < (int) _components.size(); i++)
        _components[i]->setId(i);

    // 环路检测
    if (auto err = check(); err.code != ErrorCode::OK)
        return err;

    std::string c_code = generateC();

    // shim 路径：优先使用 setJitIncludeDir() 设置的路径，否则按构建目录布局自动检测
    auto shim_path = []() -> std::string {
        namespace fs = std::filesystem;
        if (!_jit_include_dir.empty() && fs::exists(_jit_include_dir))
            return _jit_include_dir;
        auto dir = fs::path(exeDir());
        auto shim = dir / "shim";
        if (fs::exists(shim))
            return shim.string();
        shim = dir.parent_path().parent_path() / "shim";
        if (fs::exists(shim))
            return shim.string();
        return "";
    }();

    // 各元件要暴露给生成代码的符号（回调函数指针、状态对象地址等）
    std::vector<std::pair<std::string, void *>> symbols;
    for (auto &comp: _components)
        for (auto &[name, ptr]: comp->extraJitSymbols())
            symbols.emplace_back(name, ptr);

    auto res = _backend.compile(c_code, shim_path, this, symbols);
    if (res.code != ErrorCode::OK)
        return {res.code, -1, {}, res.message};

    _tick_fn = res.tick;
    _init_fn = res.init;
    _deinit_fn = res.deinit;
    _reset_fn = res.reset;
    _set_wire_fn = res.set_wire;
    _get_wire_fn = res.get_wire;
    _compiled = true;
    return {ErrorCode::OK};
}

// ============================================================
// 仿真运行
// ============================================================

void Circuit::init() {
    // 先全局初始化，再 JIT 初始化（JIT 中会调 create/reset）
    for (auto &comp: _components)
        comp->simInit();
    if (_init_fn)
        _init_fn();
}
void Circuit::tick() {
    if (_tick_error.code != ErrorCode::OK)
        return;
    if (_tick_fn)
        _tick_fn();
}

void Circuit::onBusConflict(int net_id) {
    if (_tick_error.code != ErrorCode::OK)
        return;
    // net_id < 0 是阶段 2b 迭代到上限仍未收敛的信号：三态总线绕成的组合环查不出来，
    // 只能靠迭代上限兜底，这里把它报出来而不是静默继续
    if (net_id < 0) {
        _tick_error.code = ErrorCode::BUS_OSCILLATION;
        _tick_error.message = "总线级联在迭代上限内未收敛: 电路可能存在经由三态总线的组合环";
        return;
    }
    _tick_error.code = ErrorCode::BUS_CONFLICT;
    _tick_error.net_id = net_id;
    auto it = _bus_nets.find(net_id);
    if (it != _bus_nets.end())
        for (auto &d: it->second)
            _tick_error.comp_ids.push_back(d.comp->id());
    _tick_error.message = std::format("总线冲突: 线网 '{}' 被 {} 个三态门同时驱动", _nets[net_id]->name(),
                                      _tick_error.comp_ids.size());
}

void Circuit::setWire(int id, const std::vector<uint8_t> &value) {
    if (!_set_wire_fn)
        return;
    uint8_t buf[16] = {};
    for (size_t i = 0; i < value.size() && i < 16; i++)
        buf[i] = value[i];
    _set_wire_fn(id, buf);
}

std::vector<uint8_t> Circuit::getWire(int id) {
    std::vector<uint8_t> val(16, 0);
    if (_get_wire_fn)
        _get_wire_fn(id, val.data());
    return val;
}

// ============================================================
// JSON 序列化
// ============================================================

std::string Circuit::exportJson() const {
    using json = nlohmann::json;

    json root;

    // 共享定义库
    json j_defs = json::parse(CircuitLibrary::instance().exportJson());
    if (!j_defs.empty())
        root["definitions"] = j_defs;

    // 线网: 名字数组
    json j_nets = json::array();
    for (auto &n: _nets)
        j_nets.push_back(n->name());

    // 元件
    json j_comps = json::array();
    for (auto &c: _components) {
        json jc;
        jc["type"] = c->typeName();
        jc["name"] = c->name();

        // 构造参数（复合元件导出 definition 引用）
        auto params = c->exportParams();
        if (!params.empty()) {
            json jp = json::object();
            for (auto &[k, v]: params)
                jp[k] = v;
            jc["params"] = jp;
        }

        // 引脚连接: pin_name → net_name (null 表示悬空)
        json jconn = json::object();
        for (auto &p: c->inputs()) {
            if (p->net())
                jconn[p->name()] = p->net()->name();
            else
                jconn[p->name()] = nullptr;
        }
        for (auto &p: c->outputs()) {
            if (p->net())
                jconn[p->name()] = p->net()->name();
            else
                jconn[p->name()] = nullptr;
        }
        jc["connections"] = jconn;

        j_comps.push_back(jc);
    }

    root["nets"] = j_nets;
    root["components"] = j_comps;
    return root.dump(2);
}

std::unique_ptr<Circuit> Circuit::fromJson(const std::string &json_str, std::string &error,
                                           const std::string &base_dir) {
    using json = nlohmann::json;

    json root;
    try {
        root = json::parse(json_str);
    } catch (const json::parse_error &e) {
        error = std::format("JSON 解析失败: {}", e.what());
        return nullptr;
    }

    auto circuit = std::make_unique<Circuit>();

    // 1. 创建线网，记录名字→Net*
    std::unordered_map<std::string, Net *> net_map;
    if (root.contains("nets") && root["nets"].is_array()) {
        for (auto &jn: root["nets"]) {
            std::string name = jn.get<std::string>();
            Net *net = circuit->createNet(name);
            net_map[name] = net;
        }
    }

    // 辅助: 获取或创建线网（遇到未知名字自动创建）
    auto get_or_create_net = [&](const std::string &name) -> Net * {
        if (name.empty())
            return nullptr;
        auto it = net_map.find(name);
        if (it != net_map.end())
            return it->second;
        Net *net = circuit->createNet(name);
        net_map[name] = net;
        return net;
    };

    // 2. 导入共享定义（必须在创建实例前）
    if (root.contains("definitions"))
        CircuitLibrary::instance().importJson(root["definitions"].dump(), error);

    // 3. 创建元件
    if (!root.contains("components") || !root["components"].is_array()) {
        error = "JSON 缺少 components 数组";
        return nullptr;
    }

    auto &factory = ComponentFactory::instance();

    for (auto &jc: root["components"]) {
        std::string type = jc.value("type", "");
        std::string name = jc.value("name", "");

        if (!factory.hasType(type)) {
            error = std::format("未知元件类型: {}", type);
            return nullptr;
        }

        // 收集参数
        std::unordered_map<std::string, std::string> params;
        if (jc.contains("params") && jc["params"].is_object()) {
            for (auto &[k, v]: jc["params"].items())
                params[k] = v.is_string() ? v.get<std::string>() : v.dump();
        }
        // DLL 路径相对于 JSON 文件所在目录
        if (type == "dll" && !base_dir.empty()) {
            auto it = params.find("path");
            if (it != params.end()) {
                std::filesystem::path p(it->second);
                if (p.is_relative())
                    it->second = (std::filesystem::path(base_dir) / p).string();
            }
        }

        std::string create_error;
        std::unique_ptr<Component> comp = factory.create(type, name, params, create_error);
        if (!comp) {
            error = create_error.empty() ? std::format("创建元件失败: type={}, name={}", type, name) : create_error;
            return nullptr;
        }

        Component *comp_ptr = circuit->addComponent(std::move(comp));

        // 连接引脚
        if (jc.contains("connections") && jc["connections"].is_object()) {
            for (auto &[pin_name, jnet]: jc["connections"].items()) {
                std::string net_name;
                if (jnet.is_null())
                    continue; // 悬空引脚跳过
                else if (jnet.is_string())
                    net_name = jnet.get<std::string>();
                else
                    continue;

                Net *net = get_or_create_net(net_name);
                if (net)
                    circuit->connect(comp_ptr, pin_name, net);
            }
        }
    }

    return circuit;
}

} // namespace dsc
