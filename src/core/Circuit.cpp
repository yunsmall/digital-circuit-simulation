#include "dcs/Circuit.h"
#include "dcs/CodeGen.h"
#include "dcs/CircuitSerializer.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/Context.h"
#include "dcs/components/platform/Composite.h"
#include "dcs/util/ExePath.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

// JIT 头文件搜索路径的旧接口：等价于操作默认上下文的设置，保留给老调用方
void dsc::Circuit::setJitIncludeDir(const std::string &dir) {
    Context::global().setJitIncludeDir(dir);
}

const std::string &dsc::Circuit::jitIncludeDir() {
    return Context::global().jitIncludeDir();
}

dsc::Context &dsc::Circuit::context() {
    return _context ? *_context : Context::global();
}

const dsc::Context &dsc::Circuit::context() const {
    return _context ? *_context : Context::global();
}

// 编译器后端（LLVM/TCC）相关的东西都在 JitBackend.cpp，这里不需要任何后端头

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

// ============================================================
// Circuit 构造/析构
// ============================================================

Circuit::Circuit() = default;

Circuit::~Circuit() {
    // 先放掉 JIT 里持有的外部资源（circuit_deinit 的 dcs_dll_destroy、
    // DllComponent 的句柄引用计数），再拆电路。clear() 自己也做这一步，
    // 所以"用户直接 clear() 重建"同样能归还资源
    deinit();
    clear();
}

void Circuit::deinit() {
    // 销毁元件状态（JIT circuit_deinit 中调用 dcs_dll_destroy）。
    // 用完置空：clear() 内部也会调 deinit()，用户显式 deinit() 之后再 clear()
    // 就会第二次执行 circuit_deinit，去访问已经被 simDeinit 卸载的 DLL 状态对象
    if (_jit.deinit) {
        _jit.deinit();
        _jit.deinit = nullptr;
    }

    // 仿真结束通知（DLL: dcs_dll_deinit）。各元件自己保证可重复调用
    for (auto &comp: _components)
        comp->simDeinit();
}

void Circuit::clear() {
    // 先跑 deinit：它要用 JIT 里的 circuit_deinit 和元件对象，清掉 JIT/元件之后就跑不了了。
    // deinit 内部会置空 _deinit_fn，元件侧保证 simDeinit 可重复调用，所以析构路径
    // （先 deinit 再 clear）不会重复执行
    deinit();
    jitCleanup();
    _nets.clear();
    _components.clear();
    _bus_nets.clear();
}

void Circuit::reset() {
    _tick_error = {};
    if (_jit.reset)
        _jit.reset();
}

void Circuit::jitCleanup() {
    // 仅清理 JIT 状态（函数指针 + 后端资源）
    _jit = {};
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

Component *Circuit::addComponent(std::unique_ptr<Component> comp, CircuitError *error) {
    // 加入电路时立即准备元件（DLL 加载、读取描述符创建引脚等）
    std::string prep_err;
    if (!comp->prepare(context(), prep_err)) {
        if (error)
            *error = {ErrorCode::PREPARE_FAILED, -1, {}, prep_err};
        return nullptr;
    }
    // 用"现有最大 ID + 1"而不是 size()：removeComponent 不重编号，
    // 按 size 分配会和留下来的元件撞号（编译前按 id 查元件/生成符号都会歧义）
    int next_id = 0;
    for (auto &c: _components)
        next_id = std::max(next_id, c->id() + 1);
    comp->setId(next_id);
    Component *ptr = comp.get();
    _components.push_back(std::move(comp));
    _compiled = false; // 元件集合变了，旧的 JIT 代码不再对应当前电路
    return ptr;
}

CircuitError Circuit::connect(Component *comp, const std::string &pin_name, Net *net) {
    Pin *pin = findPin(comp, pin_name);
    if (!pin)
        return {ErrorCode::PIN_NOT_FOUND, -1, {comp->id()},
                std::format("元件 '{}' 没有引脚 '{}'", comp->name(), pin_name)};

    // ---- 只读检查：这一步不能改动任何电路状态 ----
    // 失败时旧连接必须原封不动，否则调用方拿到错误码也没法恢复到连接前的状态
    int this_out_idx = -1;
    std::vector<BusDriver> shared; // 目标线网上已有的其他驱动源（走到这里必然都是三态）
    if (pin->isOutput()) {
        for (int i = 0; i < (int) comp->outputs().size(); i++)
            if (comp->outputs()[i].get() == pin) {
                this_out_idx = i;
                break;
            }

        for (auto &c: _components) {
            for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
                auto &p = c->outputs()[oi];
                if (p->net() != net || p.get() == pin)
                    continue;
                // 非三态输出恒驱动，和任何别的输出共线都是短路。
                // 注意这里不能因为"当前双方都是三态"就提前 goto——同一根线上可能
                // 还挂着第三个非三态输出，必须遍历完才能下结论
                if (!pin->isTriState() || !p->isTriState())
                    return {ErrorCode::MULTIPLE_DRIVERS, net->id(), {comp->id(), c->id()},
                            std::format("线网 '{}' 已被多个输出驱动：'{}' 和 '{}' 都不是三态输出",
                                        net->name(), comp->name(), c->name())};
                shared.push_back({c.get(), oi});
            }
        }
    }

    // ---- 检查全部通过，才动电路状态 ----
    // 改接别的线网前先断开：否则旧线网的总线驱动记录会留在 _bus_nets 里，
    // 生成代码仍把它当成旧线网的驱动源，而新线网根本不知道有这个驱动
    if (pin->net() && pin->net() != net)
        disconnect(comp, pin_name);

    if (!shared.empty()) {
        auto &drivers = _bus_nets[net->id()];
        auto add_driver = [&](Component *drv, int oidx) {
            for (auto &d : drivers)
                if (d.comp == drv && d.out_idx == oidx)
                    return;
            drivers.push_back({drv, oidx});
        };
        for (auto &d: shared)
            add_driver(d.comp, d.out_idx);
        add_driver(comp, this_out_idx);
    }

    pin->setNet(net);
    net->updateBitWidth(pin->bit_width());
    _compiled = false; // 连接关系变了，生成代码里的读写顺序需要重新生成
    return {ErrorCode::OK};
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
    // 只接受本电路里的 net：外来 Net* 的 id 可能恰好等于本电路某根线网的 id，
    // 直接按 id 删 _bus_nets 会误删本电路的总线驱动记录，而且不报错
    if (std::find_if(_nets.begin(), _nets.end(),
                     [net](const auto &uptr) { return uptr.get() == net; }) == _nets.end())
        return;
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
    // 不能按 _components[id] 取：removeComponent 之后别的元件 ID 不会立刻重排，
    // 下标和 ID 只在 validate()/compile() 重编号后才一致
    for (auto &c: _components)
        if (c->id() == id)
            return c.get();
    return nullptr;
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

CircuitError Circuit::prepareNetlist(std::vector<std::string> *warnings) {
    if (auto err = expandComposites(); err.code != ErrorCode::OK)
        return err;
    // 展开后重新编号，保证 ID = 索引
    for (int i = 0; i < (int) _components.size(); i++)
        _components[i]->setId(i);
    return check(warnings);
}

CircuitError Circuit::validate(std::vector<std::string> *warnings) {
    // 展开复合元件后才能检查子电路；这里不生成 C、不碰 JIT
    jitCleanup();
    return prepareNetlist(warnings);
}

CircuitError Circuit::check(std::vector<std::string> *warnings) {
    // setNet() 是公开接口，可能把引脚挂到不属于本电路的 Net 上。
    // 在入口统一扫一遍，后面的所有下标访问（driver_count、adj）就不必逐个兜底
    for (auto &comp: _components) {
        auto bad_net = [&](const std::unique_ptr<Pin> &p) {
            Net *n = p->net();
            return n && (n->id() < 0 || n->id() >= (int) _nets.size());
        };
        for (auto &p: comp->inputs())
            if (bad_net(p))
                return {ErrorCode::INVALID_NET, p->net()->id(), {comp->id()},
                        std::format("元件 '{}' 的引脚 '{}' 连到了不属于本电路的线网", comp->name(), p->name())};
        for (auto &p: comp->outputs())
            if (bad_net(p))
                return {ErrorCode::INVALID_NET, p->net()->id(), {comp->id()},
                        std::format("元件 '{}' 的引脚 '{}' 连到了不属于本电路的线网", comp->name(), p->name())};
    }

    // 非三态输出严格单驱动：两个非三态输出接同一根线就是短路。
    // connect() 已经拒绝这种连接，但反序列化、直接 setNet、以后的编辑接口都能绕过它；
    // 而直写 _w[] 的优化元件（CPU、WishboneMemory）完全依赖"这根线只有我一个驱动"
    // 这个前提。在这里按最终结构证明一遍，前提不成立就拒绝编译。
    //
    // 这里独立统计全部输出驱动，不复用 _bus_nets：后者只记录"多个三态输出共享"的线网，
    // 单驱动线网和"三态 + 非三态"冲突都不在里面，拿来判多驱动会漏
    {
        std::vector<int> driver_count(_nets.size(), 0);
        for (auto &comp: _components)
            for (auto &p: comp->outputs())
                if (p->net())
                    driver_count[p->net()->id()]++;
        for (auto &comp: _components) {
            for (auto &p: comp->outputs()) {
                Net *n = p->net();
                if (!n || p->isTriState())
                    continue;
                if (driver_count[n->id()] > 1)
                    return {ErrorCode::MULTIPLE_DRIVERS, n->id(), {comp->id()},
                            std::format("线网 '{}' 有 {} 个输出驱动，非三态输出不允许与其他输出共线",
                                        n->name(), driver_count[n->id()])};
            }
        }
    }

    // 元件自己的接线检查：比如互连的某个从设备端口后面根本没接设备，访问那段地址
    // 只会让主设备永远等 ack。这属于接错线，不是电路非法，所以只告警、不拦编译。
    // 收集和打印分开：检查函数只负责填 warnings，CLI/compile 决定怎么呈现
    for (auto &comp: _components) {
        std::string msg = comp->checkTopology(*this);
        if (msg.empty())
            continue;
        if (warnings)
            warnings->push_back(std::format("元件 '{}': {}", comp->name(), msg));
    }

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

CircuitError Circuit::expandComposites() {
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
            // 展开前记录快照：失败时要把半成品撤干净。否则用户修好 JSON 再对同一个
            // Circuit 调 compile()，会再次展开同一个复合元件——重名子元件、重名线网、
            // 重复连接
            const size_t comp_before = _components.size();
            const size_t net_before = _nets.size();
            std::vector<Net *> in_nets, out_nets;
            for (auto &p: comp->inputs())
                in_nets.push_back(p->net());
            for (auto &p: comp->outputs())
                out_nets.push_back(p->net());

            CompositeComponent::Expansion exp;
            comp->expandTo(*this, exp);

            if (!exp.error.empty()) {
                // 先断开新增子元件（顺带清理 _bus_nets 里它们的记录），再销毁
                for (size_t i = comp_before; i < _components.size(); i++)
                    disconnectAll(_components[i].get());
                while (_components.size() > comp_before)
                    _components.pop_back();
                // 展开时新建的线网都追加在尾部，父电路原有的在 net_before 之前
                while (_nets.size() > net_before)
                    _nets.pop_back();
                // 恢复复合元件自己被解绑的引脚
                for (size_t i = 0; i < comp->inputs().size(); i++)
                    comp->inputs()[i]->setNet(in_nets[i]);
                for (size_t i = 0; i < comp->outputs().size(); i++)
                    comp->outputs()[i]->setNet(out_nets[i]);
                _compiled = false;
                return {ErrorCode::PREPARE_FAILED, -1, {}, exp.error};
            }

            found = true;
            auto it = std::find_if(_components.begin(), _components.end(), [comp](auto &c) { return c.get() == comp; });
            if (it != _components.end()) {
                _components.erase(it);
            }
        }
    }
    return {ErrorCode::OK};
}

// ============================================================
// C 代码生成（实现在 CodeGen.cpp）
// ============================================================

std::string Circuit::generateC() const {
    return generateCircuitC(const_cast<Circuit *>(this), _nets, _components, _bus_nets);
}

CircuitError Circuit::compile() {
    // 先把入口指针全部置空再清理后端：cleanup() 会释放已编译的代码，
    // 若本次编译失败，残留的旧函数指针就成了悬空指针
    jitCleanup();
    // 上一个电路留下的总线冲突/振荡错误不能让新编译的电路一 tick 就静默停摆
    _tick_error = {};

    std::vector<std::string> warnings;
    if (auto err = prepareNetlist(&warnings); err.code != ErrorCode::OK)
        return err;
    for (auto &w: warnings)
        std::cerr << std::format("[dcs] 警告: {}\n", w);

    std::string c_code = generateC();

    // 调试用：把生成的 C 落到文件里，方便对照反汇编排查性能问题
    if (const char *dump = std::getenv("DCS_DUMP_C")) {
        std::ofstream(dump) << c_code;
    }

    // shim 路径：优先使用上下文的 setJitIncludeDir() 设置，否则按构建目录布局自动检测
    auto shim_path = [this]() -> std::string {
        namespace fs = std::filesystem;
        const std::string &include_dir = context().jitIncludeDir();
        if (!include_dir.empty() && fs::exists(include_dir))
            return include_dir;
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

    // JIT 的符号表按名字建索引，同名只会保留最后注册的那一个。两个元件把同一个名字
    // 注册成不同地址时，生成代码会调到另一家的回调、还拿对方的状态指针来解释，
    // 表现是宿主进程莫名崩溃。同址重复是正常的（多个元件导出同一个全局函数）
    std::unordered_map<std::string, void *> symbol_owner;
    for (auto &[name, ptr]: symbols) {
        auto [it, inserted] = symbol_owner.emplace(name, ptr);
        if (!inserted && it->second != ptr)
            return {ErrorCode::JIT_SYMBOL_FAILED, -1, {},
                    std::format("JIT 符号名冲突: '{}' 被注册成不同地址", name)};
    }

    auto res = _backend.compile(c_code, shim_path, this, symbols);
    if (res.code != ErrorCode::OK)
        return {res.code, -1, {}, res.message};

    _jit = res;
    _compiled = true;
    return {ErrorCode::OK};
}


// ============================================================
// 仿真运行
// ============================================================

void Circuit::init() {
    // 初始化等于一次冷启动，上一个电路残留的运行期错误不能带进新状态
    _tick_error = {};
    // 先全局初始化，再 JIT 初始化（JIT 中会调 create/reset）
    for (auto &comp: _components)
        comp->simInit();
    if (_jit.init)
        _jit.init();
}
void Circuit::tick() {
    // 编译后改了结构只会清 _compiled，旧 JIT 还在：直接跑会用过期代码读写
    // 已经变过的 net/元件编号，结果静默出错。没编译好就当没有电路
    if (!_compiled)
        return;
    if (_tick_error.code != ErrorCode::OK)
        return;
    if (_jit.tick)
        _jit.tick();
}

void Circuit::tickCycle(int clk_net_id) {
    if (!_compiled)
        return;
    if (_tick_error.code != ErrorCode::OK)
        return;
    if (_jit.tick_cycle) {
        _jit.tick_cycle(clk_net_id);
        return;
    }
    setWire(clk_net_id, 0);
    tick();
    setWire(clk_net_id, 1);
    tick();
}

void Circuit::tickCycles(int clk_net_id, uint64_t n) {
    if (!_compiled)
        return;
    if (_tick_error.code != ErrorCode::OK)
        return;
    if (_jit.tick_cycles) {
        _jit.tick_cycles(clk_net_id, n);
        return;
    }
    for (uint64_t i = 0; i < n; i++)
        tickCycle(clk_net_id);
}

void Circuit::tickBatch(uint64_t n) {
    if (!_compiled)
        return;
    if (_tick_error.code != ErrorCode::OK)
        return;
    if (_jit.tick_batch) {
        _jit.tick_batch(n);
        return;
    }
    for (uint64_t i = 0; i < n; i++)
        tick();
}

void Circuit::tickForever() {
    if (!_compiled)
        return;
    if (_jit.tick_forever)
        _jit.tick_forever();
    for (;;)
        tick();
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
    if (!_compiled || !_jit.set_wire)
        return;
    uint8_t buf[16] = {};
    for (size_t i = 0; i < value.size() && i < 16; i++)
        buf[i] = value[i];
    _jit.set_wire(id, buf);
}

void Circuit::setWire(int id, uint64_t value) {
    if (!_compiled)
        return;
    if (_jit.set_wire_u64) {
        _jit.set_wire_u64(id, value);
        return;
    }
    if (!_jit.set_wire)
        return;
    uint8_t buf[16] = {};
    std::memcpy(buf, &value, sizeof(value));
    _jit.set_wire(id, buf);
}

std::vector<uint8_t> Circuit::getWire(int id) {
    std::vector<uint8_t> val(16, 0);
    if (_compiled && _jit.get_wire)
        _jit.get_wire(id, val.data());
    return val;
}

// ============================================================
// JSON 序列化
// ============================================================

std::string Circuit::exportJson() const {
    return exportCircuitJson(*this);
}


std::unique_ptr<Circuit> Circuit::fromJson(const std::string &json_str, std::string &error,
                                            const std::string &base_dir, Context *context) {
    return loadCircuitFromJson(json_str, error, base_dir, context);
}

} // namespace dsc