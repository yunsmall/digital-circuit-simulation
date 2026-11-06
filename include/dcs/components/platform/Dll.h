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
#include <cstdint>
#include <string>
#include <vector>
#include "dcs/Component.h"
#include "dcs/dll_interface.h"

namespace dsc {

class DllComponent : public Component {
public:
    DllComponent(const std::string &name, const std::string &dll_path);
    // 兜住只调 ensurePins 的路径（--pins、GUI 引脚查询）：它们不会走 simDeinit
    ~DllComponent() override;

    // --- Component 接口 ---
    // 逻辑类型在加载 DLL 时确定并记住：simDeinit() 会清空函数指针，
    // 但 validate() 可能在 deinit 之后调用，只按指针判断会把 DLL 当成无逻辑元件，
    // 漏掉组合环检测
    bool hasCombinational() const override {
        return _has_comb;
    }
    bool hasSequential() const override {
        return _has_seq;
    }
    // 引脚和函数指针都来自 DLL，只能加载后才知道；ensurePins 幂等
    bool ensurePins(Context &ctx, std::string &error) override;
    bool prepareForCompile(Context &ctx, std::string &error) override;
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
    // 只加载描述符和函数指针，不创建引脚。prepare() 首次建引脚时调用；
    // simInit() 在 deinit 之后重新调用（引脚已存在，不能重复创建）
    bool loadSymbols(std::string &error);

    std::string _dll_path;
    void *_dll_handle = nullptr;
    bool _loaded = false;
    // 加载后记录的能力位；函数指针在 simDeinit 后会清空，这两个标志保留
    bool _has_comb = false;
    bool _has_seq = false;
    // 只有真正执行过 dcs_dll_init 才能执行 dcs_dll_deinit。
    // validate()/--check 只 prepare 不 simInit，销毁时不能调用 deinit
    bool _init_called = false;

    dcs_dll_descriptor_t _desc = {};

    dcs_dll_init_t _init_fn = nullptr;
    dcs_dll_deinit_t _deinit_fn = nullptr;
    dcs_dll_create_t _create_fn = nullptr;
    dcs_dll_destroy_t _destroy_fn = nullptr;
    dcs_dll_tick_t _tick_comb_fn = nullptr;
    dcs_dll_tick_t _tick_seq_fn = nullptr;
    dcs_dll_reset_t _reset_fn = nullptr;

    // 可选导出：每个输入引脚的边沿依赖（DCS_DLL_INPUT_EDGES）。
    // 指针指向 DLL 内部，卸载后必须清空，处理方式和 _desc 相同
    const uint8_t *_input_edges = nullptr;
    // 可选导出：包装层自己合成伪时钟沿的能力（DCS_DLL_SET_PSEUDO）
    dcs_dll_set_pseudo_t _set_pseudo_fn = nullptr;

    // 生成一个阶段的 tick 函数体（comb 或 seq 公用）
    std::string genTickFunc(const char *phase_suffix) const;
    // 清空从 DLL 取到的全部函数指针（卸载或加载失败后共用）
    void resetDllPointers();
    // 该输入是否接在伪时钟线上（电平恒 0，必须由包装层合成沿）
    bool inputIsPseudoClock(int index) const;
    // 生成 C 代码时用的伪时钟掩码，没有伪时钟输入时返回空
    std::vector<uint8_t> pseudoMask() const;
};

// 校验 DLL 导出的描述符本身能不能用：描述符来自外部二进制，被损坏或恶意构造时
// 可能出现负数量、空数组、空引脚名，直接解引用就是崩溃/UB。--dll-info 也走这里
bool validateDllDescriptor(const dcs_dll_descriptor_t &desc, std::string &error);

} // namespace dsc
