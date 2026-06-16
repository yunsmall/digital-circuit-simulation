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

namespace dsc {

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

    // 1. 加载 DLL
#ifdef _WIN32
    _dll_handle = LoadLibraryA(_dll_path.c_str());
    if (!_dll_handle) {
        error = std::format("无法加载 DLL: {}", _dll_path);
        return false;
    }
    auto loadSym = [&](const char *name, auto &fn_ptr) -> bool {
        FARPROC fp = GetProcAddress((HMODULE) _dll_handle, name);
        if (!fp) {
            error = std::format("DLL 缺少导出符号: {}", name);
            return false;
        }
        memcpy(&fn_ptr, &fp, sizeof(fp));
        return true;
    };
#else
    _dll_handle = dlopen(_dll_path.c_str(), RTLD_NOW);
    if (!_dll_handle) {
        error = std::format("无法加载 DLL: {} ({})", _dll_path, dlerror());
        return false;
    }
    auto loadSym = [&](const char *name, auto &fn_ptr) -> bool {
        void *sym = dlsym(_dll_handle, name);
        if (!sym) {
            error = std::format("DLL 缺少导出符号: {}", name);
            return false;
        }
        memcpy(&fn_ptr, &sym, sizeof(sym));
        return true;
    };
#endif

    // 2. 读取描述符（引脚配置）
    dcs_dll_descriptor_t *desc_ptr = nullptr;
    if (!loadSym(DCS_DLL_DESC, desc_ptr))
        return false;
    _desc = *desc_ptr; // 复制描述符结构体（引脚数组指针指向 DLL 内静态内存）

    // 3. 根据描述符自动创建引脚
    for (int i = 0; i < _desc.num_inputs; i++)
        addInput(_desc.inputs[i].name, _desc.inputs[i].bit_width);
    for (int i = 0; i < _desc.num_outputs; i++)
        addOutput(_desc.outputs[i].name, _desc.outputs[i].bit_width);

    // 4. 加载生命周期函数
    if (!loadSym(DCS_DLL_INIT, _init_fn))
        return false;
    if (!loadSym(DCS_DLL_DEINIT, _deinit_fn))
        return false;
    if (!loadSym(DCS_DLL_CREATE, _create_fn))
        return false;
    if (!loadSym(DCS_DLL_DESTROY, _destroy_fn))
        return false;
    if (!loadSym(DCS_DLL_TICK, _tick_fn))
        return false;
    if (!loadSym(DCS_DLL_RESET, _reset_fn))
        return false;

    _loaded = true;
    return true;
}

// ============================================================
// 生命周期钩子
// ============================================================

void DllComponent::simInit() {
    if (_init_fn)
        _init_fn();
}

void DllComponent::simDeinit() {
    if (_deinit_fn)
        _deinit_fn();
}

void DllComponent::onCleanup() {
#ifdef _WIN32
    if (_dll_handle)
        FreeLibrary((HMODULE) _dll_handle);
#else
    if (_dll_handle)
        dlclose(_dll_handle);
#endif
    _dll_handle = nullptr;
    _loaded = false;
}

// ============================================================
// extraJitSymbols — 让生成的 C 代码能调用 DLL 函数
// ============================================================

std::vector<Component::JitSymbol> DllComponent::extraJitSymbols() const {
    int id = _id;
    return {
            {std::format("_dcs_dll_create_{}", id), reinterpret_cast<void *>(_create_fn)},
            {std::format("_dcs_dll_destroy_{}", id), reinterpret_cast<void *>(_destroy_fn)},
            {std::format("_dcs_dll_tick_{}", id), reinterpret_cast<void *>(_tick_fn)},
            {std::format("_dcs_dll_reset_{}", id), reinterpret_cast<void *>(_reset_fn)},
    };
}

// ============================================================
// C 代码生成
// ============================================================

std::string DllComponent::genFuncDef() const {
    int id = _id;
    int ni = (int) inputs().size();
    int no = (int) outputs().size();

    // 输入：gen_read_wire 声明变量 → 复制到 _dll_in 缓冲区
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

    // 输出：声明临时变量 → 从 _dll_out 复制 → gen_write_wire 写回线网
    std::string out_decls, copys_out, writes;
    for (int i = 0; i < no; i++) {
        int nid = outputs()[i]->netId();
        int bw = outputs()[i]->bit_width();
        int bytes = byte_count(bw);
        std::string tmp = std::format("_dll_wr_{}", i);
        out_decls += std::format("    {} {}; dcs_memset(&{}, 0, {});\n", c_int_type(bw), tmp, tmp, bytes);
        copys_out += std::format("    dcs_memcpy(&{}, _dll_out[{}], {});\n", tmp, i, bytes);
        writes += "    " + gen_write_wire(nid, tmp, bw) + "\n";
    }

    int in_count = ni > 0 ? ni : 1;
    int out_count = no > 0 ? no : 1;
    std::string code;
    code += std::format("// DLL 元件: {}\n", name());
    code += std::format("extern void* _dcs_dll_create_{0}(void);\n", id);
    code += std::format("extern void  _dcs_dll_destroy_{0}(void*);\n", id);
    code += std::format("extern void  _dcs_dll_tick_{0}(void*, const uint8_t*, uint8_t*);\n", id);
    code += std::format("extern void  _dcs_dll_reset_{0}(void*, uint8_t*);\n", id);
    code += std::format("\nstatic void* _dcs_dll_state_{0};\n\n", id);
    code += std::format("static void {}(void) {{\n", funcName());
    code += reads;
    code += out_decls;
    code += std::format("    uint8_t _dll_in[{}][16];\n", in_count);
    code += std::format("    uint8_t _dll_out[{}][16];\n", out_count);
    code += "    dcs_memset(_dll_in, 0, sizeof(_dll_in));\n";
    code += "    dcs_memset(_dll_out, 0, sizeof(_dll_out));\n";
    code += copys_in;
    code += std::format("    _dcs_dll_tick_{0}(_dcs_dll_state_{0}, (const uint8_t*)_dll_in, (uint8_t*)_dll_out);\n",
                        id);
    code += copys_out;
    code += writes;
    code += "}\n";
    return code;
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

std::string DllComponent::genDeinitCode() const {
    return std::format(R"(
    _dcs_dll_destroy_{0}(_dcs_dll_state_{0});)",
                       _id);
}

std::unique_ptr<Component> DllComponent::clone(const std::string &new_name) const {
    return std::make_unique<DllComponent>(new_name, _dll_path);
}

} // namespace dsc
