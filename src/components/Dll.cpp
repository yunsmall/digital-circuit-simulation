// DllComponent — 加载外部 DLL 实现的自定义元件
#include "dcs/components/Dll.h"

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

// ============================================================
// prepare — 加载 DLL、读取描述符、创建引脚
// ============================================================

bool DllComponent::prepare(std::string &error) {
    if (_loaded)
        return true;

    _dll_handle = dll_load(_dll_path, error);
    if (!_dll_handle)
        return false;

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

    for (int i = 0; i < _desc.num_inputs; i++)
        addInput(_desc.inputs[i].name, _desc.inputs[i].bit_width);
    for (int i = 0; i < _desc.num_outputs; i++) {
        auto &od = _desc.outputs[i];
        addOutput(od.name, od.bit_width, od.is_tri_state != 0);
        _outputs.back()->setSequential(od.is_sequential != 0);
    }

    // 必须导出生命周期函数
    if (!loadSym(DCS_DLL_INIT, _init_fn)) {
        error = "DLL 缺少: dcs_dll_init";
        return false;
    }
    if (!loadSym(DCS_DLL_DEINIT, _deinit_fn)) {
        error = "DLL 缺少: dcs_dll_deinit";
        return false;
    }
    if (!loadSym(DCS_DLL_CREATE, _create_fn)) {
        error = "DLL 缺少: dcs_dll_create";
        return false;
    }
    if (!loadSym(DCS_DLL_DESTROY, _destroy_fn)) {
        error = "DLL 缺少: dcs_dll_destroy";
        return false;
    }
    if (!loadSym(DCS_DLL_RESET, _reset_fn)) {
        error = "DLL 缺少: dcs_dll_reset";
        return false;
    }

    // tick_comb 和 tick_seq 至少加载一个
    loadSym(DCS_DLL_TICK_COMB, _tick_comb_fn);
    loadSym(DCS_DLL_TICK_SEQ, _tick_seq_fn);
    if (!_tick_comb_fn && !_tick_seq_fn) {
        error = "DLL 必须至少导出 dcs_dll_tick_comb 或 dcs_dll_tick_seq";
        return false;
    }

    _loaded = true;
    return true;
}

// ============================================================
// 生命周期
// ============================================================

void DllComponent::simInit() {
    if (_init_fn)
        _init_fn();
}

void DllComponent::simDeinit() {
    if (_deinit_fn)
        _deinit_fn();
    if (_loaded) {
        dll_unload(_dll_path);
        _dll_handle = nullptr;
        _loaded = false;
    }
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

std::string DllComponent::genTickFunc(const char *phase, int fn_id_offset) const {
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
    }

    int in_count = ni > 0 ? ni : 1;
    int out_count = no > 0 ? no : 1;
    std::string code;
    code += std::format("// DLL 元件: {} ({})\n", name(), phase);
    code += std::format("extern void* _dcs_dll_create_{0}(void);\n", id);
    code += std::format("extern void  _dcs_dll_destroy_{0}(void*);\n", id);
    code += std::format("extern void  _dcs_dll_tick_{0}_{1}(void*, const uint8_t*, uint8_t*);\n", phase, id);
    code += std::format("extern void  _dcs_dll_reset_{0}(void*, uint8_t*);\n", id);
    code += std::format("\nstatic void* _dcs_dll_state_{0};\n\n", id);
    code += std::format("static void _update_{}_{}(void) {{\n", id, phase);
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
    return genTickFunc("comb", 0);
}

std::string DllComponent::genFuncDef_seq() const {
    if (!_tick_seq_fn)
        return "";
    return genTickFunc("seq", 0);
}

std::string DllComponent::genInitCode() const {
    int id = _id;
    int no = (int) outputs().size();
    int out_count = no > 0 ? no : 1;
    std::string code;
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
    return std::format(R"(
    _dcs_dll_destroy_{0}(_dcs_dll_state_{0});)",
                       _id);
}

std::unique_ptr<Component> DllComponent::clone(const std::string &new_name) const {
    return std::make_unique<DllComponent>(new_name, _dll_path);
}

} // namespace dsc
