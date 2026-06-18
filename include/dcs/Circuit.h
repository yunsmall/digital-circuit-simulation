#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Component.h"
#include "Net.h"

#ifndef DCS_USE_TCC
// LLVM/Clang 类型前向声明（仅 LLVM 后端需要，TCC 后端不引入 LLVM 依赖）
namespace llvm {
class LLVMContext;
namespace orc {
    class LLJIT;
}
} // namespace llvm
#endif

namespace dsc {

// ============================================================
// 运行时错误
// ============================================================

enum class ErrorCode {
    OK = 0,
    PIN_NOT_FOUND,
    MULTIPLE_DRIVERS,
    COMBINATIONAL_LOOP,
    PREPARE_FAILED, // DLL 加载等编译前准备失败
    JIT_COMPILE_FAILED, // C→IR 编译失败（TCC/Clang 统一）
    JIT_LINK_FAILED, // 重定位/添加模块失败
    JIT_SYMBOL_FAILED, // 符号注入/查找失败
    BUS_CONFLICT, // 运行时总线冲突
};

struct CircuitError {
    ErrorCode code = ErrorCode::OK;
    int net_id = -1;
    std::vector<int> comp_ids; // 参与冲突的元件 ID
    std::string message;
};

// ============================================================
// Circuit — 电路容器 + JIT 编译 + 仿真运行
//
// 用户唯一需要操作的接口类。支持两种 JIT 后端:
//   - LLVM/Clang (默认): cmake -B build
//   - TinyCC (轻量):    cmake -B build_tcc -DDCS_USE_TCC=ON
//
// 典型使用流程:
//   1. 构建电路:
//        auto* n = c.createNet("net1");
//        auto* g = c.addComponent(std::make_unique<LogicGate>("g1", 2, 8));
//        c.connect(g, "in0", n);
//   2. 编译:  c.compile();   // 环路检测 → 展开复合 → 生成C → JIT编译
//   3. 仿真:  c.init(); c.setWire(id, val); c.tick(); c.getWire(id);
//   4. 重建:  c.clear();     // 清空电路和编译结果，可重新构建
//
// 线程安全: 不支持并发，所有操作用户负责同步
// ============================================================
class Circuit {
public:
    Circuit();
    ~Circuit();

    // --- 构建电路 ---
    // 创建线网，位宽由后续连接的引脚自动确定（取最大值）
    Net *createNet(const std::string &name);
    // 添加元件，返回原始指针供后续 connect() 使用
    Component *addComponent(std::unique_ptr<Component> comp);
    // 连接引脚到线网，自动检查多驱动冲突、更新线网位宽
    void connect(Component *comp, const std::string &pin_name, Net *net);

    // 断开引脚与线网的连接（已断开时无操作）。输出引脚会自动清理 _bus_nets 三态驱动记录
    void disconnect(Component *comp, const std::string &pin_name);

    // 断开元件的全部引脚（先输出后输入，保证 _bus_nets 正确清理）
    void disconnectAll(Component *comp);

    // 移除元件：断开全部引脚并从电路中销毁。调用后原 Component* 失效
    void removeComponent(Component *comp);

    // 移除线网：断开所有连接到此线网的引脚，再从电路中移除并销毁
    void removeNet(Net *net);

    // ⚠️ Component 析构前，若其引脚仍连接在线网上，必须先调用 disconnect，
    // 否则 Net 中会残留悬空指针、_bus_nets 中留有脏数据

    // --- 验证与编译 ---
    CircuitError check(); // 环路检测，失败返回错误
    CircuitError compile(); // JIT 编译（内部: 展开复合→检测→生成C→编译→链接）
    bool isCompiled() const {
        return _compiled;
    }

    // --- 仿真运行 ---
    void init(); // 调用 circuit_init()，清零所有状态
    void tick(); // 调用 circuit_tick()，执行一个周期。有未清错误时直接返回
    void reset(); // 重置仿真状态（仅清零线网和时序状态，不重建 DLL 实例）
    void deinit(); // 结束仿真（销毁 DLL 状态、调用 deinit 等）
    void setWire(int id, const std::vector<uint8_t> &value); // 设置线网值
    std::vector<uint8_t> getWire(int id); // 读取线网值

    // --- 运行时错误 ---
    bool hasError() const {
        return _tick_error.code != ErrorCode::OK;
    }
    const CircuitError &tickError() const {
        return _tick_error;
    }
    void clearError() {
        _tick_error = {};
    }
    // 由 JIT 回调（勿手动调用）
    void onBusConflict(int net_id);

    // --- 重建 ---
    void clear(); // 清空电路和 JIT 编译结果

    // --- 查询 ---
    const auto &nets() const {
        return _nets;
    }
    const auto &components() const {
        return _components;
    }
    int netCount() const {
        return (int) _nets.size();
    }
    int componentCount() const {
        return (int) _components.size();
    }
    Net *findNet(const std::string &name) const;
    Component *componentById(int id) const; // 按 ID 查找元件

    // --- 调试 ---
    std::string generateC() const; // 生成等效 C 代码，不触发编译

    // --- JSON 序列化 ---
    std::string exportJson() const; // 导出电路为 JSON 字符串
    static std::unique_ptr<Circuit> fromJson(const std::string &json, std::string &error,
                                             const std::string &base_dir = ""); // 从 JSON 重建电路

    // --- JIT 头文件搜索路径 ---
    // 设置后优先使用；为空时使用默认行为（exe 同目录下的 shim/）
    static void setJitIncludeDir(const std::string &dir);
    static const std::string &jitIncludeDir();

private:
    enum class Color { WHITE, GRAY, BLACK };
    std::vector<std::unique_ptr<Net>> _nets;
    std::vector<std::unique_ptr<Component>> _components;

    // 总线追踪：net_id → [(comp, out_idx), ...]（三态输出允许多驱动）
    struct BusDriver {
        Component *comp;
        int out_idx;
    };
    std::map<int, std::vector<BusDriver>> _bus_nets;

    bool _compiled = false;
    CircuitError _tick_error;

#ifndef DCS_USE_TCC
    std::unique_ptr<llvm::LLVMContext> _llvm_ctx;
    std::unique_ptr<llvm::orc::LLJIT> _jit;
#endif
    void *_tcc_state = nullptr; // TCCState* (TCC 后端使用)

    static std::string _jit_include_dir; // JIT 头文件搜索路径（空=自动检测）

    void (*_tick_fn)() = nullptr;
    void (*_init_fn)() = nullptr;
    void (*_deinit_fn)() = nullptr;
    void (*_reset_fn)() = nullptr;
    void (*_set_wire_fn)(int, const uint8_t *) = nullptr;
    void (*_get_wire_fn)(int, uint8_t *) = nullptr;

    Pin *findPin(Component *comp, const std::string &name);
    void jitCleanup();
    void expandComposites();
    bool dfsCycle(int u, const std::vector<std::vector<int>> &adj, std::vector<Color> &color, std::vector<int> &path);
    std::vector<Component *> topologicalSort(const std::vector<Component *> &comb) const;
};

} // namespace dsc
