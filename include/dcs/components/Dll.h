// DllComponent — 加载外部 DLL 实现的自定义元件
//
// 用户编写 DLL（按 dcs_dll_interface.h 导出 dcs_dll_desc 描述符和生命周期函数），
// DllComponent 自动根据描述符创建引脚、生成 JIT 调用代码。
//
// 用法:
//   auto dl = std::make_unique<DllComponent>("my", "my_dll.dll");
//   c.addComponent(std::move(dl));
//   // 引脚由 DLL 描述符自动创建，无需手动添加
#pragma once
#include <string>
#include "dcs/Component.h"
#include "dcs/dll_interface.h"

namespace dsc {

class DllComponent : public Component {
public:
    // dll_path: DLL 文件路径（编译时加载）
    DllComponent(const std::string &name, const std::string &dll_path);

    // --- Component 接口 ---
    bool isSequential() const override {
        return true;
    }
    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    void simInit() override; // 仿真启动 → dcs_dll_init
    void simDeinit() override; // 仿真结束 → dcs_dll_deinit
    void onCleanup() override; // 元件移除 → FreeLibrary
    std::string genFuncDef() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

private:
    std::string _dll_path;
    void *_dll_handle = nullptr;
    bool _loaded = false;

    dcs_dll_descriptor_t _desc = {};

    dcs_dll_init_t _init_fn = nullptr;
    dcs_dll_deinit_t _deinit_fn = nullptr;
    dcs_dll_create_t _create_fn = nullptr;
    dcs_dll_destroy_t _destroy_fn = nullptr;
    dcs_dll_tick_t _tick_fn = nullptr;
    dcs_dll_reset_t _reset_fn = nullptr;
};

} // namespace dsc
