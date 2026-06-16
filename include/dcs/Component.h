#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "Pin.h"

namespace dsc {

// ============================================================
// C 代码生成工具函数 — 实现在 src/Component.cpp
// 各元件的 genFuncDef() 调用这些函数生成 C 代码片段
// ============================================================

// 根据位宽返回 C 整数类型名（1~8→uint8_t, 9~16→uint16_t, 17~32→uint32_t, 33~64→uint64_t）
const char *c_int_type(int bit_width);

// 位宽对应的字节数（向上取整）
int byte_count(int bit_width);

// 生成 C 位掩码字面量（如 "0xFFULL"）
std::string gen_mask(int width);

// 生成"从线网读取值"的 C 代码片段，悬空输入自动初始化为 0，超宽自动加掩码
std::string gen_read_wire(int net_id, int pin_w, int net_w, std::string_view var);

// 生成"写入线网值"的 C 代码片段，自动清零超出位宽的字节
std::string gen_write_wire(int net_id, std::string_view val, int pin_w);

// ============================================================
// 元件抽象基类
//
// 所有电路元件的基类。通过虚方法返回 C 代码字符串片段，
// 供 Circuit 拼接成完整的仿真程序。
//
// 派生体系:
//   Component
//   ├── CombinationalComponent   (组合逻辑，无状态)
//   │   └── MultiInputGate<>     (多输入门 CRTP 模板)
//   └── SequentialComponent      (时序逻辑，有独立状态结构体)
// ============================================================

class Component {
public:
    // type_name: 元件类型名（如 "and"、"dff"），用于生成 C 函数名
    Component(const std::string &name, const std::string &type_name);
    virtual ~Component() = default;

    const std::string &name() const {
        return _name;
    }
    const std::string &typeName() const {
        return _type_name;
    }
    const auto &inputs() const {
        return _inputs;
    }
    const auto &outputs() const {
        return _outputs;
    }

    // 是否为时序元件。用于: ①环路检测（时序输出不产生组合环路） ②tick() 排序
    virtual bool isSequential() const {
        return false;
    }

    // 环路检测：返回输出引脚 out_idx 的组合逻辑依赖输入索引列表
    // 组合逻辑返回所有输入，时序逻辑返回空（输出由内部状态决定）
    virtual std::vector<int> getCombinationalDeps(int) const;

    // ======== C 代码生成虚方法 ========

    // 状态结构体定义（如 DFF: typedef struct { uint8_t q[16]; ... } _S_dff_xxx;）
    virtual std::string genStructDef() const {
        return "";
    }

    // 状态变量声明（如 static _S_dff_xxx _s_dff_xxx;）
    virtual std::string genStateDecl() const {
        return "";
    }

    // 更新函数完整定义（函数签名 + 函数体），每个元件必须实现
    virtual std::string genFuncDef() const = 0;

    // 初始化代码片段，插入到 circuit_init() 函数体中
    virtual std::string genInitCode() const {
        return "";
    }

    // 反初始化代码片段，插入到 circuit_deinit() 函数体中（DLL 状态销毁等）
    virtual std::string genDeinitCode() const {
        return "";
    }

    // 函数前向声明（如 static void _update_and_g1(void);）
    virtual std::string genFuncDecl() const;

    // 生成唯一 C 函数名: _update_<type>_<name>
    virtual std::string funcName() const;

    // 状态变量名: _s_<type>_<name>    状态类型名: _S_<type>_<name>
    std::string stateVarName() const;
    std::string stateTypeName() const;

    // 克隆元件（保留配置参数和引脚信息，仅改名称）。用于复合元件展开
    virtual std::unique_ptr<Component> clone(const std::string &new_name) const = 0;

    // 是否为复合元件（内部包含子电路），编译前需要展开
    virtual bool isComposite() const {
        return false;
    }

    // 电路内唯一 ID（由 Circuit::addComponent 分配）
    int id() const {
        return _id;
    }
    void setId(int id) {
        _id = id;
    }

    // ======== 外部资源 & JIT 符号 ========

    // 编译前准备（加载 DLL 等外部资源），返回 false 则编译中止
    virtual bool prepare(std::string & /*error*/) {
        return true;
    }

    // 需要注入 JIT 的额外符号（DLL 函数指针等），电路编译时统一注入
    struct JitSymbol {
        std::string name;
        void *ptr;
    };
    virtual std::vector<JitSymbol> extraJitSymbols() const {
        return {};
    }

    // 仿真启动时调用（DLL: 调用 dcs_dll_init）
    virtual void simInit() {
    }

    // 仿真结束时调用（DLL: 调用 dcs_dll_deinit）
    virtual void simDeinit() {
    }

    // 编译成功后调用（一次性初始化，如分配资源）
    virtual void onCompiled() {
    }

    // JIT 编译+重定位完成后调用（Memory: 写入内存指针到 JIT 变量）
    // lookup 用于查找 JIT 中的符号地址
    virtual void onJitReady(void *(*lookup)(void *, const char *), void *ctx) {
    }

    // 元件从电路移除时调用（DLL: FreeLibrary）
    virtual void onCleanup() {
    }

    // JSON 序列化：导出构造参数
    virtual std::unordered_map<std::string, std::string> exportParams() const {
        return _params;
    }

protected:
    void setParam(const std::string &key, const std::string &value) {
        _params[key] = value;
    }
    // 子类用：创建输入/输出引脚
    Pin *addInput(const std::string &pin_name, int bit_width);
    Pin *addOutput(const std::string &pin_name, int bit_width);

    std::string _name;
    std::string _type_name;
    int _id = -1; // 电路内唯一 ID，由 Circuit::addComponent 分配
    std::vector<std::unique_ptr<Pin>> _inputs;
    std::vector<std::unique_ptr<Pin>> _outputs;
    std::unordered_map<std::string, std::string> _params;
};

// 组合逻辑元件基类 — isSequential() 永远返回 false，无状态结构体
class CombinationalComponent : public Component {
public:
    using Component::Component;
    bool isSequential() const final {
        return false;
    }
    std::string genStructDef() const final {
        return "";
    }
    std::string genStateDecl() const final {
        return "";
    }
    std::string genInitCode() const final {
        return "";
    }
};

// 时序逻辑元件基类 — isSequential() 永远返回 true，环路检测不产生边
class SequentialComponent : public Component {
public:
    using Component::Component;
    bool isSequential() const final {
        return true;
    }
    std::vector<int> getCombinationalDeps(int) const final {
        return {};
    }
};

} // namespace dsc
