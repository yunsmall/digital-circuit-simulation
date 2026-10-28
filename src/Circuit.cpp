#include "dcs/Circuit.h"
#include "dcs/CodeGen.h"
#include "dcs/CircuitSerializer.h"
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


std::string Circuit::generateC() const {
    return generateCircuitC(const_cast<Circuit *>(this), _nets, _components, _bus_nets);
}

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
    return loadCircuitFromJson(json_str, error, base_dir);
}

} // namespace dsc