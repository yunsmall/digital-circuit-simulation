#include "dcs/Circuit.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/ComponentFactory.h"
#include "dcs/components/Composite.h"
#include "dcs/util/ExePath.h"

#include <algorithm>
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
    std::string prep_err;
    if (!comp->prepare(prep_err))
        throw std::runtime_error(prep_err);
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
    oss << "    // 阶段1: 组合逻辑\n";
    for (auto *c: sorted)
        if (c->hasCombinational())
            oss << "    " << c->funcName_comb() << "();\n";
    oss << "    // 阶段2: 传播 + 总线决议\n";

    // 传播阶段：_c_out → _w（含总线决议）
    // 先收集哪些线网是总线，哪些普通线网已经被处理
    std::set<int> bus_nets_handled;

    // 总线决议
    for (auto &[net_id, drivers]: _bus_nets) {
        int nb = byte_count(_nets[net_id]->bit_width());
        oss << "\n    // 总线 " << net_id << "（" << drivers.size() << " 个三态驱动）\n";
        oss << "    {\n";
        oss << "        int _active = -1;\n";
        for (int di = 0; di < (int) drivers.size(); di++) {
            int cid = drivers[di].comp->id();
            int oi = drivers[di].out_idx;
            if (di == 0)
                oss << std::format("        if (_c_oe_{}_{}) _active = {};\n", cid, oi, di);
            else
                oss << std::format(
                        "        if (_c_oe_{}_{}) {{\n"
                        "            if (_active >= 0) {{ dcs_bus_conflict(_circuit_ptr, {}); goto _bus_done_{}; }}\n"
                        "            _active = {};\n"
                        "        }}\n",
                        cid, oi, net_id, net_id, di);
        }
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
        oss << std::format("    _bus_done_{}: ;\n", net_id);
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
    // 仅重置 JIT 函数指针，不卸载 DLL
    _tick_fn = nullptr;
    _init_fn = nullptr;
    _deinit_fn = nullptr;
    _reset_fn = nullptr;
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
    if (auto err = check(); err.code != ErrorCode::OK)
        return err;

    std::string c_code = generateC();


    // 获取 shim 路径：优先使用 setJitIncludeDir() 设置的路径，否则自动检测
    auto shim_path = []() -> std::string {
        namespace fs = std::filesystem;
        // 优先：用户通过 setJitIncludeDir() 设置
        if (!_jit_include_dir.empty()) {
            if (fs::exists(_jit_include_dir))
                return _jit_include_dir;
        }
        // 回退：exe 同目录下的 shim/
        auto dir = fs::path(exeDir());
        auto shim = dir / "shim";
        if (fs::exists(shim))
            return shim.string();
        // 再回退：exe 上两级（开发构建目录结构）
        shim = dir.parent_path().parent_path() / "shim";
        if (fs::exists(shim))
            return shim.string();
        // 再回退：安装布局（bin/dcs → ../include/dcs/shim）
        shim = dir.parent_path() / "include" / "dcs" / "shim";
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
        return {ErrorCode::JIT_COMPILE_FAILED, -1, {}, "tcc_new 失败"};
    }

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    // 不使用标准库，纯独立 C 代码
    tcc_set_options(s, "-nostdlib -O2");

    if (!shim_path.empty()) {
        tcc_add_sysinclude_path(s, shim_path.c_str());
    }

    // 统一入口：编译前注入所有元件的额外 JIT 符号（DLL 函数指针、内存地址等）
    tcc_add_symbol(s, "dcs_bus_conflict",
                   reinterpret_cast<void *>(reinterpret_cast<void (*)(void *, int)>(&dcs_bus_conflict)));
    for (auto &comp: _components)
        for (auto &[name, ptr]: comp->extraJitSymbols())
            tcc_add_symbol(s, name.c_str(), ptr);

    if (tcc_compile_string(s, c_code.c_str()) == -1) {
        tcc_delete(s);
        return {ErrorCode::JIT_COMPILE_FAILED, -1, {}, "TCC 编译失败"};
    }

    // 重定位（TCC HEAD 及 Windows 均为单参数；旧版 apt 需双参数 tcc_relocate(s, TCC_RELOCATE_AUTO)）
    tcc_relocate(s);

    // 查找函数符号
    _tick_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_tick"));
    _init_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_init"));
    _deinit_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_deinit"));
    _reset_fn = reinterpret_cast<void (*)()>(tcc_get_symbol(s, "circuit_reset"));
    _set_wire_fn = reinterpret_cast<void (*)(int, const uint8_t *)>(tcc_get_symbol(s, "circuit_set_wire"));
    _get_wire_fn = reinterpret_cast<void (*)(int, uint8_t *)>(tcc_get_symbol(s, "circuit_get_wire"));

    if (!_tick_fn || !_init_fn || !_set_wire_fn || !_get_wire_fn) {
        tcc_delete(s);
        return {ErrorCode::JIT_SYMBOL_FAILED, -1, {}, "TCC 查找符号失败"};
    }

    _tcc_state = s;
    _compiled = true;
    return {ErrorCode::OK};

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
    invoc.getTargetOpts().CPU = "x86-64"; // 基线 x86-64，避免 CI VM 中 SIGILL
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
        jitCleanup();
        return {ErrorCode::JIT_COMPILE_FAILED, -1, {}, "Clang 编译 C 代码失败"};
    }
    auto module = action->takeModule();

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        jitCleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, {}, "检测目标机失败: " + llvm::toString(jtmb.takeError())};
    }
    jtmb->setCPU("x86-64"); // 基线 x86-64，避免 CI VM 中 SIGILL
    auto jit_or_err = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit_or_err) {
        auto llvm_err = "创建 LLJIT 失败: " + llvm::toString(jit_or_err.takeError());
        jitCleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, {}, llvm_err};
    }
    _jit = std::move(*jit_or_err);

    // 统一入口：注入所有元件的额外 JIT 符号（DLL 函数指针等）
    {
        llvm::orc::SymbolMap syms;
        // 注入总线冲突回调
        syms[_jit->mangleAndIntern("dcs_bus_conflict")] = {
                llvm::orc::ExecutorAddr::fromPtr(
                        reinterpret_cast<void *>(reinterpret_cast<void (*)(void *, int)>(&dcs_bus_conflict))),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
        for (auto &comp: _components)
            for (auto &[name, ptr]: comp->extraJitSymbols())
                syms[_jit->mangleAndIntern(name)] = {llvm::orc::ExecutorAddr::fromPtr(ptr),
                                                     llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
        if (auto err = _jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            auto sym_err = "注入 JIT 符号失败: " + llvm::toString(std::move(err));
            jitCleanup();
            return {ErrorCode::JIT_SYMBOL_FAILED, -1, {}, sym_err};
        }
    }

    auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::make_unique<llvm::LLVMContext>());
    if (auto err = _jit->addIRModule(std::move(tsm))) {
        auto mod_err = "添加 IR 模块失败: " + llvm::toString(std::move(err));
        jitCleanup();
        return {ErrorCode::JIT_LINK_FAILED, -1, {}, mod_err};
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
    _reset_fn = lookup.operator()<void (*)()>("circuit_reset");
    _set_wire_fn = lookup.operator()<void (*)(int, const uint8_t *)>("circuit_set_wire");
    _get_wire_fn = lookup.operator()<void (*)(int, uint8_t *)>("circuit_get_wire");

    if (!_tick_fn || !_init_fn || !_set_wire_fn || !_get_wire_fn) {
        jitCleanup();
        return {ErrorCode::JIT_SYMBOL_FAILED, -1, {}, "查找 JIT 符号失败"};
    }
    _compiled = true;
    return {ErrorCode::OK};
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
    if (_tick_error.code != ErrorCode::OK)
        return;
    if (_tick_fn)
        _tick_fn();
}

void Circuit::onBusConflict(int net_id) {
    if (_tick_error.code != ErrorCode::OK)
        return;
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
