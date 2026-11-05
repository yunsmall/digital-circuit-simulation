// DllComponent — 加载外部 DLL 实现的自定义元件
#include "dcs/ComponentFactory.h"
#include "dcs/CodeGenNames.h"
#include "dcs/Context.h"
#include "dcs/components/platform/Dll.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstring>
#include <format>
#include <stdexcept>
#include <unordered_map>

namespace dsc {

// DLL 句柄引用计数管理（同一 DLL 被多个元件共享时不会重复加载/卸载）
namespace {
    struct DllHandle {
        void *handle = nullptr;
        int refcount = 0;
    };
    static std::unordered_map<std::string, DllHandle> &dll_cache() {
        static std::unordered_map<std::string, DllHandle> cache;
        return cache;
    }

    static void *dll_load(const std::string &path, std::string &error) {
        auto &cache = dll_cache();
        auto it = cache.find(path);
        if (it != cache.end()) {
            it->second.refcount++;
            return it->second.handle;
        }
#ifdef _WIN32
        void *h = LoadLibraryA(path.c_str());
#else
        void *h = dlopen(path.c_str(), RTLD_NOW);
#endif
        if (!h) {
            error = std::format("无法加载 DLL: {}", path);
            return nullptr;
        }
        cache[path] = {h, 1};
        return h;
    }

    static void dll_unload(const std::string &path) {
        auto &cache = dll_cache();
        auto it = cache.find(path);
        if (it == cache.end())
            return;
        if (--it->second.refcount > 0)
            return;
#ifdef _WIN32
        FreeLibrary((HMODULE) it->second.handle);
#else
        dlclose(it->second.handle);
#endif
        cache.erase(it);
    }
} // namespace

DllComponent::DllComponent(const std::string &name, const std::string &dll_path) :
    Component(name, "dll"), _dll_path(dll_path) {
    setParam("path", _dll_path);
}

DllComponent::~DllComponent() {
    // 只调 ensurePins 的路径（--pins、GUI 引脚查询）不会走 simDeinit，
    // 句柄引用必须在这里归还；正常电路 simDeinit 已把 _loaded 清掉，不会重复卸载
    if (_loaded) {
        dll_unload(_dll_path);
        _dll_handle = nullptr;
        _loaded = false;
    }
}

// ============================================================
// ensurePins — 加载 DLL、读取描述符、创建引脚
// ============================================================

// 只加载描述符和函数指针，不创建引脚。ensurePins() 首次建引脚时调用；
// simDeinit 之后再 simInit 也走这里（引脚已存在，不能重复创建）
bool DllComponent::loadSymbols(std::string &error) {
    // 重新加载可能换了 DLL 内容，能力位以这次加载为准
    _has_comb = false;
    _has_seq = false;
#ifdef _WIN32
    auto loadSym = [&](const char *name, auto &fn_ptr) -> bool {
        FARPROC fp = GetProcAddress((HMODULE) _dll_handle, name);
        if (!fp)
            return false;
        std::memcpy(&fn_ptr, &fp, sizeof(fp));
        return true;
    };
#else
    auto loadSym = [&](const char *name, auto &fn_ptr) -> bool {
        void *sym = dlsym(_dll_handle, name);
        if (!sym)
            return false;
        std::memcpy(&fn_ptr, &sym, sizeof(sym));
        return true;
    };
#endif

    dcs_dll_descriptor_t *desc_ptr = nullptr;
    if (!loadSym(DCS_DLL_DESC, desc_ptr)) {
        error = "DLL 缺少导出符号: dcs_dll_desc";
        return false;
    }
    _desc = *desc_ptr;

    // 描述符里的数组指针指向 DLL 内部；后面任何符号缺失都会导致调用方卸载
    // DLL，此时必须把描述符清回零值，不能留下悬空指针
    auto need = [&](const char *sym, auto &fn_ptr) -> bool {
        if (loadSym(sym, fn_ptr))
            return true;
        _desc = {};
        error = std::format("DLL 缺少: {}", sym);
        return false;
    };

    if (!need(DCS_DLL_INIT, _init_fn))
        return false;
    if (!need(DCS_DLL_DEINIT, _deinit_fn))
        return false;
    if (!need(DCS_DLL_CREATE, _create_fn))
        return false;
    if (!need(DCS_DLL_DESTROY, _destroy_fn))
        return false;
    if (!need(DCS_DLL_RESET, _reset_fn))
        return false;

    // tick_comb 和 tick_seq 至少加载一个
    loadSym(DCS_DLL_TICK_COMB, _tick_comb_fn);
    loadSym(DCS_DLL_TICK_SEQ, _tick_seq_fn);
    _has_comb = _tick_comb_fn != nullptr;
    _has_seq = _tick_seq_fn != nullptr;
    if (!_tick_comb_fn && !_tick_seq_fn) {
        _desc = {};
        error = "DLL 必须至少导出 dcs_dll_tick_comb 或 dcs_dll_tick_seq";
        return false;
    }
    _loaded = true;
    return true;
}

bool DllComponent::ensurePins(Context &ctx, std::string &error) {
    // 解析后的绝对路径要留住：simInit/simDeinit 重新加载时还要用
    if (!_loaded && !_dll_path.empty())
        _dll_path = ctx.resolvePath(_dll_path);
    if (_loaded)
        return true;

    _dll_handle = dll_load(_dll_path, error);
    if (!_dll_handle)
        return false;

    std::string sym_err;
    if (!loadSymbols(sym_err)) {
        // 失败必须归还引用计数：否则这个 DLL 的 refcount 只增不减，
        // 同一 DLL 的其他实例 deinit 时也降不到 0，句柄永久泄漏
        dll_unload(_dll_path);
        _dll_handle = nullptr;
        error = sym_err;
        return false;
    }

    // 引脚描述来自 DLL（外部输入）：位宽非法时 addInput 里的 assert 在 Release
    // 构建不生效，会拖到生成 C 代码才炸。先整体校验，再建引脚
    auto fail = [&](std::string msg) {
        dll_unload(_dll_path);
        _dll_handle = nullptr;
        _loaded = false;
        error = std::move(msg);
        return false;
    };
    for (int i = 0; i < _desc.num_inputs; i++) {
        const int bw = _desc.inputs[i].bit_width;
        if (bw < 1 || bw > 128)
            return fail(std::format("DLL 输入 '{}' 的位宽 {} 无效（1..128）", _desc.inputs[i].name, bw));
    }
    for (int i = 0; i < _desc.num_outputs; i++) {
        const int bw = _desc.outputs[i].bit_width;
        if (bw < 1 || bw > 128)
            return fail(std::format("DLL 输出 '{}' 的位宽 {} 无效（1..128）", _desc.outputs[i].name, bw));
    }

    for (int i = 0; i < _desc.num_inputs; i++)
        addInput(_desc.inputs[i].name, _desc.inputs[i].bit_width);
    for (int i = 0; i < _desc.num_outputs; i++) {
        auto &od = _desc.outputs[i];
        addOutput(od.name, od.bit_width, od.is_tri_state != 0);
        _outputs.back()->setSequential(od.is_sequential != 0);
    }
    return true;
}

// deinit() 卸载了 DLL 并把函数指针清空；重新编译时 JIT 符号表和代码生成
// 都要求地址有效。这里只重新加载符号，引脚已经由 prepare() 建好，不能再创建
bool DllComponent::prepareForCompile(Context &, std::string &error) {
    if (_loaded)
        return true;
    _dll_handle = dll_load(_dll_path, error);
    if (!_dll_handle)
        return false;
    if (!loadSymbols(error)) {
        dll_unload(_dll_path);
        _dll_handle = nullptr;
        // 部分符号可能已经取到地址，卸载后全部失效；编译失败也不能留下悬空指针
        _init_fn = nullptr;
        _deinit_fn = nullptr;
        _create_fn = nullptr;
        _destroy_fn = nullptr;
        _reset_fn = nullptr;
        _tick_comb_fn = nullptr;
        _tick_seq_fn = nullptr;
        return false;
    }
    return true;
}

// ============================================================
// 生命周期
// ============================================================

void DllComponent::simInit() {
    // deinit 之后又 init：重新加载 DLL 和函数指针。引脚在 prepare() 时已经建好，
    // 这里不能走 prepare（会重复 addInput/addOutput）
    if (!_loaded) {
        std::string err;
        _dll_handle = dll_load(_dll_path, err);
        if (!_dll_handle)
            return;
        if (!loadSymbols(err)) {
            dll_unload(_dll_path);
            _dll_handle = nullptr;
            return;
        }
    }
    // 重复 init（用户把 init 当"重置"用）不能重复执行全局 init；
    // deinit 会把 _init_called 清掉，下一次 init 才会重新执行
    if (_init_called)
        return;
    if (_init_fn) {
        _init_fn();
        _init_called = true;
    }
}

void DllComponent::simDeinit() {
    // validate()/--check 路径只 prepare，从未 simInit；这时不能调 deinit，
    // 否则 DLL 会在 init 之前收到 deinit
    if (_deinit_fn && _init_called)
        _deinit_fn();
    _init_called = false;
    if (_loaded) {
        dll_unload(_dll_path);
        _dll_handle = nullptr;
        _loaded = false;
    }
    // 卸载后这些指针指向已释放的代码页。不清空的话，重复 init/deinit
    // （测试、交互式仿真器里很常见）会在下一次 simInit 时调用悬空地址
    _init_fn = nullptr;
    _deinit_fn = nullptr;
    _create_fn = nullptr;
    _destroy_fn = nullptr;
    _reset_fn = nullptr;
    _tick_comb_fn = nullptr;
    _tick_seq_fn = nullptr;
}

// ============================================================
// extraJitSymbols
// ============================================================

std::vector<Component::JitSymbol> DllComponent::extraJitSymbols() const {
    int id = _id;
    std::vector<JitSymbol> syms = {
            {std::format("_dcs_dll_create_{}", id), reinterpret_cast<void *>(_create_fn)},
            {std::format("_dcs_dll_destroy_{}", id), reinterpret_cast<void *>(_destroy_fn)},
            {std::format("_dcs_dll_reset_{}", id), reinterpret_cast<void *>(_reset_fn)},
    };
    if (_tick_comb_fn)
        syms.push_back({std::format("_dcs_dll_tick_comb_{}", id), reinterpret_cast<void *>(_tick_comb_fn)});
    if (_tick_seq_fn)
        syms.push_back({std::format("_dcs_dll_tick_seq_{}", id), reinterpret_cast<void *>(_tick_seq_fn)});
    return syms;
}

// ============================================================
// C 代码生成
// ============================================================

std::string DllComponent::genTickFunc(const char *phase) const {
    int id = _id;
    int ni = (int) inputs().size();
    int no = (int) outputs().size();

    std::string reads, copys_in;
    for (int i = 0; i < ni; i++) {
        int nid = inputs()[i]->netId();
        int nw = nid >= 0 ? inputs()[i]->net()->bit_width() : 0;
        int bw = inputs()[i]->bit_width();
        int bytes = byte_count(bw);
        std::string tmp = std::format("_dll_rd_{}", i);
        reads += "    " + gen_read_wire(nid, bw, nw, tmp) + "\n";
        copys_in += std::format("    dcs_memcpy(_dll_in[{}], &{}, {});\n", i, tmp, bytes);
    }

    std::string out_decls, copys_out, writes;
    for (int i = 0; i < no; i++) {
        int nid = outputs()[i]->netId();
        int bw = outputs()[i]->bit_width();
        int bytes = byte_count(bw);
        std::string tmp = std::format("_dll_wr_{}", i);
        out_decls += std::format("    {} {}; dcs_memset(&{}, 0, {});\n", c_int_type(bw), tmp, tmp, bytes);
        copys_out += std::format("    dcs_memcpy(&{}, _dll_out[{}], {});\n", tmp, i, bytes);
        writes += "    " + genOutputWrite(i, tmp, bw) + "\n";
        // DLL 接口没有动态控制三态使能的手段，所以输出一律视为常驱动；
        // 不写这一行的话，声明为三态的输出 _c_oe 会一直是初始的 false，
        // 生成的总线决议会把它当高阻，下游永远读不到值
        writes += std::format("    {} = true;\n", cgen::cOe(id, i));
    }

    int in_count = ni > 0 ? ni : 1;
    int out_count = no > 0 ? no : 1;
    std::string code;
    code += std::format("// DLL 元件: {} ({})\n", cgen::commentText(name()), phase);
    code += std::format("extern void* _dcs_dll_create_{0}(void);\n", id);
    code += std::format("extern void  _dcs_dll_destroy_{0}(void*);\n", id);
    code += std::format("extern void  _dcs_dll_tick_{0}_{1}(void*, const uint8_t*, uint8_t*);\n", phase, id);
    code += std::format("extern void  _dcs_dll_reset_{0}(void*, uint8_t*);\n", id);
    code += std::format("\nstatic void* _dcs_dll_state_{0};\n\n", id);
    // 函数名必须和基类 genFuncDecl_comb/seq 生成的前向声明一致
    code += std::format("static void {}(void) {{\n", std::string_view(phase) == "comb" ? funcName_comb() : funcName_seq());
    code += reads;
    code += out_decls;
    code += std::format("    uint8_t _dll_in[{}][16];\n", in_count);
    code += std::format("    uint8_t _dll_out[{}][16];\n", out_count);
    code += "    dcs_memset(_dll_in, 0, sizeof(_dll_in));\n";
    code += "    dcs_memset(_dll_out, 0, sizeof(_dll_out));\n";
    code += copys_in;
    code += std::format("    _dcs_dll_tick_{0}_{1}(_dcs_dll_state_{1}, "
                        "(const uint8_t*)_dll_in, (uint8_t*)_dll_out);\n",
                        phase, id);
    code += copys_out;
    code += writes;
    code += "}\n";
    return code;
}

std::string DllComponent::genFuncDef_comb() const {
    if (!_tick_comb_fn)
        return "";
    return genTickFunc("comb");
}

std::string DllComponent::genFuncDef_seq() const {
    if (!_tick_seq_fn)
        return "";
    return genTickFunc("seq");
}

std::string DllComponent::genInitCode() const {
    int id = _id;
    int no = (int) outputs().size();
    int out_count = no > 0 ? no : 1;
    std::string code;
    // circuit_init 可能被重复调用（init 当重置用）：先销毁旧状态，避免泄漏
    code += std::format("    if (_dcs_dll_state_{0}) _dcs_dll_destroy_{0}(_dcs_dll_state_{0});\n", id);
    code += std::format("    _dcs_dll_state_{0} = _dcs_dll_create_{0}();\n", id);
    code += "    {\n";
    code += std::format("        uint8_t _out[{}][16];\n", out_count);
    code += "        dcs_memset(_out, 0, sizeof(_out));\n";
    code += std::format("        _dcs_dll_reset_{0}(_dcs_dll_state_{0}, (uint8_t*)_out);\n", id);
    code += "    }";
    return code;
}

std::string DllComponent::genResetCode() const {
    int id = _id;
    int no = (int) outputs().size();
    int out_count = no > 0 ? no : 1;
    std::string code;
    code += "    {\n";
    code += std::format("        uint8_t _out[{}][16];\n", out_count);
    code += "        dcs_memset(_out, 0, sizeof(_out));\n";
    code += std::format("        _dcs_dll_reset_{0}(_dcs_dll_state_{0}, (uint8_t*)_out);\n", id);
    code += "    }";
    return code;
}

std::string DllComponent::genDeinitCode() const {
    // 销毁后清零，重复 deinit/再 init 时不会 double-destroy
    return std::format(R"(
    if (_dcs_dll_state_{0}) {{
        _dcs_dll_destroy_{0}(_dcs_dll_state_{0});
        _dcs_dll_state_{0} = 0;
    }})",
                       _id);
}

std::unique_ptr<Component> DllComponent::clone(const std::string &new_name) const {
    return std::make_unique<DllComponent>(new_name, _dll_path);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"dll", {{"path", ""}}, {}, {}, dsc::ComponentCategory::Misc, "外部DLL元件，由动态库提供仿真逻辑"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::DllComponent>(n, dsc::paramStr(p, "path", ""));
        });
} // namespace
