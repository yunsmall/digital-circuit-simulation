// DllComponent — 加载外部 DLL 实现的自定义元件
//
// 用户编写 DLL（按 dcs_dll_interface.h 导出 dcs_dll_desc 描述符和生命周期函数），
// DllComponent 自动根据描述符创建引脚、生成 JIT 调用代码。
//
// DLL 可选导出 dcs_dll_tick_comb（组合阶段）和/或 dcs_dll_tick_seq（时序阶段），
// 二者至少导出一个。导出了哪个就参与哪个阶段。
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
    DllComponent(const std::string &name, const std::string &dll_path);

    // --- Component 接口 ---
    bool hasCombinational() const override {
        return _tick_comb_fn != nullptr;
    }
    bool hasSequential() const override {
        return _tick_seq_fn != nullptr;
    }
    bool prepare(std::string &error) override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    void simInit() override;
    void simDeinit() override;

    std::string genFuncDef_comb() const override;
    std::string genFuncDef_seq() const override;
    std::string genInitCode() const override;
    std::string genDeinitCode() const override;
    std::string genResetCode() const override;

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
    dcs_dll_tick_t _tick_comb_fn = nullptr;
    dcs_dll_tick_t _tick_seq_fn = nullptr;
    dcs_dll_reset_t _reset_fn = nullptr;

    // 生成一个阶段的 tick 函数体（comb 或 seq 公用）
    std::string genTickFunc(const char *phase_suffix, int fn_id_offset) const;
};

} // namespace dsc
