#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/Composite.h"
#include "dcs/util/ExePath.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

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
    // 卸载外部资源（DLL: FreeLibrary）
    for (auto &comp: _components)
        comp->onCleanup();

    jitCleanup();
    _nets.clear();
    _components.clear();
    _error.clear();
}

void Circuit::jitCleanup() {
    // 仅清理 JIT 状态（函数指针 + 后端资源）
    _tick_fn = nullptr;
    _init_fn = nullptr;
    _deinit_fn = nullptr;
    _set_wire_fn = nullptr;
    _get_wire_fn = nullptr;
#ifdef DCS_USE_TCC
    if (_tcc_state) {
        tcc_delete(static_cast<TCCState *>(_tcc_state));
        _tcc_state = nullptr;
    }
#else
    _jit.reset();
    _llvm_ctx.reset();
#endif
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
    if (!comp->prepare(_error))
        throw std::runtime_error(_error);
    comp->setId((int) _components.size());
    Component *ptr = comp.get();
    _components.push_back(std::move(comp));
    return ptr;
}

void Circuit::connect(Component *comp, const std::string &pin_name, Net *net) {
    Pin *pin = findPin(comp, pin_name);
    if (!pin)
        throw std::runtime_error(std::format("元件 '{}' 没有引脚 '{}'", comp->name(), pin_name));

    if (pin->isOutput()) {
        for (auto &c: _components)
            for (auto &p: c->outputs())
                if (p->net() == net && p.get() != pin)
                    throw std::runtime_error(std::format("线网 '{}' 已被多个输出驱动", net->name()));
    }
    pin->setNet(net);
    net->updateBitWidth(pin->bit_width());
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

void Circuit::check() {
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
                throw std::runtime_error(oss.str());
            }
        }
    }
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
#include "dcs_util.h"

)";
    oss << std::format("#define NET_COUNT {}\n\n", _nets.size());
    oss << "static uint8_t _w[NET_COUNT][16];\n\n";

    std::vector<Component *> comb, seq;
    for (auto &c: _components)
        (c->isSequential() ? seq : comb).push_back(c.get());

    for (auto *s: seq) {
        auto def = s->genStructDef();
        if (!def.empty())
            oss << def << "\n\n";
    }
    for (auto *s: seq) {
        auto decl = s->genStateDecl();
        if (!decl.empty())
            oss << decl << "\n";
    }
    if (!seq.empty())
        oss << "\n";

    auto sorted = topologicalSort(comb);

    for (auto *c: sorted)
        oss << c->genFuncDecl() << "\n";
    for (auto *s: seq)
        oss << s->genFuncDecl() << "\n";
    oss << "\n";

    for (auto *c: sorted)
        oss << c->genFuncDef() << "\n";
    for (auto *s: seq)
        oss << s->genFuncDef() << "\n";

    oss << "void circuit_tick(void) {\n";
    for (auto *c: sorted)
        oss << "    " << c->funcName() << "();\n";
    for (auto *s: seq)
        oss << "    " << s->funcName() << "();\n";
    oss << "}\n\n";

    oss << R"(void circuit_init(void) {
    dcs_memset(_w, 0, sizeof(_w));
)";
    for (auto *s: seq) {
        auto init = s->genInitCode();
        if (!init.empty())
            oss << init << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_deinit(void) {
)";
    for (auto *s: seq) {
        auto deinit = s->genDeinitCode();
        if (!deinit.empty())
            oss << deinit << "\n";
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

void Circuit::compile() {
    // 仅重置 JIT 函数指针，不卸载 DLL
    _tick_fn = nullptr;
    _init_fn = nullptr;
    _deinit_fn = nullptr;
    _set_wire_fn = nullptr;
    _get_wire_fn = nullptr;
    _compiled = false;
#ifdef DCS_USE_TCC
    if (_tcc_state) {
        tcc_delete(static_cast<TCCState *>(_tcc_state));
        _tcc_state = nullptr;
    }
#else
    _jit.reset();
    _llvm_ctx.reset();
#endif

    // 展开复合元件
    expandComposites();

    // 展开后重新编号，保证 ID = 索引
    for (int i = 0; i < (int) _components.size(); i++)
        _components[i]->setId(i);

    // 环路检测
    check();

    // 统一入口：所有元件的编译前准备（如加载 DLL）
    for (auto &comp: _components)
        if (!comp->prepare(_error))
            return;

    std::string c_code = generateC();

    // 获取 shim 路径
    auto shim_path = []() -> std::string {
        namespace fs = std::filesystem;
        auto dir = fs::path(exeDir());
        auto shim = dir / "shim";
        if (fs::exists(shim))
            return shim.string();
        shim = dir.parent_path().parent_path() / "shim";
        if (fs::exists(shim))
            return shim.string();
        return "";
    }();

#ifdef DCS_USE_TCC
    // ============================================================
    // TinyCC 后端
    // ============================================================
    auto *s = tcc_new();
    if (!s) {
        _error = "tcc_new 失败";
        return;
    }

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    // 不使用标准库，纯独立 C 代码
    tcc_set_options(s, "-nostdlib");

    if (!shim_path.empty()) {
        tcc_add_include_path(s, shim_path.c_str());
        // 头文件 #include <dcs_stdint.h> 需要 <> 查找，加 sysinclude
        tcc_add_sysinclude_path(s, shim_path.c_str());
    }

    // 统一入口：编译前注入所有元件的额外 JIT 符号（DLL 函数指针、内存地址等）
    for (auto &comp: _components)
        for (auto &[name, ptr]: comp->extraJitSymbols())
            tcc_add_symbol(s, name.c_str(), ptr);

    if (tcc_compile_string(s, c_code.c_str()) == -1) {
        _error = "TCC 编译失败";
        tcc_delete(s);
        return;
    }

    // 重定位
    tcc_relocate(s);

    // 查找函数符号
    _tick_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_tick"));
    _init_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_init"));
    _deinit_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_deinit"));
    _set_wire_fn = reinterpret_cast<void (*)(int, const uint8_t *)>(tcc_get_symbol(s, "circuit_set_wire"));
    _get_wire_fn = reinterpret_cast<void (*)(int, uint8_t *)>(tcc_get_symbol(s, "circuit_get_wire"));

    if (!_tick_fn || !_init_fn || !_set_wire_fn || !_get_wire_fn) {
        _error = "TCC 查找符号失败";
        tcc_delete(s);
        return;
    }

    _tcc_state = s;
    _compiled = true;

#else
    // ============================================================
    // LLVM/Clang 后端
    // ============================================================
    init_compiler_env();

    _llvm_ctx = std::make_unique<llvm::LLVMContext>();

    auto mem_buf = llvm::MemoryBuffer::getMemBuffer(c_code, "circuit.c");
    auto ci = std::make_unique<clang::CompilerInstance>();
    ci->createDiagnostics();
    auto &invoc = ci->getInvocation();
    invoc.getTargetOpts().Triple = llvm::sys::getDefaultTargetTriple();
    invoc.getLangOpts().C99 = true;
    invoc.getLangOpts().C11 = true;
    invoc.getLangOpts().CPlusPlus = false;
    invoc.getLangOpts().Bool = true;
    invoc.getLangOpts().Freestanding = true; // 不依赖标准库
    invoc.getCodeGenOpts().OptimizationLevel = 2;
    invoc.getCodeGenOpts().RelocationModel = llvm::Reloc::PIC_;

    if (!shim_path.empty()) {
        auto &hdr = invoc.getHeaderSearchOpts();
        hdr.AddPath(shim_path, clang::frontend::System, false, false);
    }

    invoc.getPreprocessorOpts().UsePredefines = true;
    invoc.getFrontendOpts().Inputs.push_back(clang::FrontendInputFile("circuit.c", clang::Language::C));
    invoc.getFrontendOpts().ProgramAction = clang::frontend::EmitLLVMOnly;
    ci->getPreprocessorOpts().addRemappedFile("circuit.c", mem_buf.release());

    auto action = std::make_unique<clang::EmitLLVMOnlyAction>(_llvm_ctx.get());
    if (!ci->ExecuteAction(*action)) {
        _error = "Clang 编译 C 代码失败";
        jitCleanup();
        return;
    }
    auto module = action->takeModule();

    auto jit_or_err = llvm::orc::LLJITBuilder().create();
    if (!jit_or_err) {
        _error = "创建 LLJIT 失败: " + llvm::toString(jit_or_err.takeError());
        jitCleanup();
        return;
    }
    _jit = std::move(*jit_or_err);

    // 统一入口：注入所有元件的额外 JIT 符号（DLL 函数指针等）
    {
        llvm::orc::SymbolMap syms;
        for (auto &comp: _components)
            for (auto &[name, ptr]: comp->extraJitSymbols())
                syms[_jit->mangleAndIntern(name)] = {llvm::orc::ExecutorAddr::fromPtr(ptr),
                                                     llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
        if (auto err = _jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            _error = "注入 JIT 符号失败: " + llvm::toString(std::move(err));
            jitCleanup();
            return;
        }
    }

    auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::make_unique<llvm::LLVMContext>());
    if (auto err = _jit->addIRModule(std::move(tsm))) {
        _error = "添加 IR 模块失败: " + llvm::toString(std::move(err));
        jitCleanup();
        return;
    }

    auto lookup = [this]<typename F>(const std::string &name) -> F {
        if (!_jit)
            return nullptr;
        auto sym = _jit->lookup(name);
        if (!sym) {
            llvm::consumeError(sym.takeError());
            return nullptr;
        }
        return sym->template toPtr<F>();
    };
    _tick_fn = lookup.operator()<void (*)()>("circuit_tick");
    _init_fn = lookup.operator()<void (*)()>("circuit_init");
    _deinit_fn = lookup.operator()<void (*)()>("circuit_deinit");
    _set_wire_fn = lookup.operator()<void (*)(int, const uint8_t *)>("circuit_set_wire");
    _get_wire_fn = lookup.operator()<void (*)(int, uint8_t *)>("circuit_get_wire");

    if (!_tick_fn || !_init_fn || !_set_wire_fn || !_get_wire_fn) {
        _error = "查找 JIT 符号失败";
        jitCleanup();
        return;
    }
    _compiled = true;
#endif
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
    if (_tick_fn)
        _tick_fn();
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

std::unique_ptr<Circuit> Circuit::fromJson(const std::string &json_str, std::string &error) {
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

        std::unique_ptr<Component> comp = factory.create(type, name, params);
        if (!comp) {
            error = std::format("创建元件失败: type={}, name={}", type, name);
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
