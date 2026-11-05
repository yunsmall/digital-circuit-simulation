#pragma once
#include "dcs/ErrorCode.h"
#include "dcs/JitBackend.h"
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "Component.h"
#include "Net.h"

namespace dsc {

class Context; // 见 dcs/Context.h：蓝图库 + JIT 头文件目录

// ============================================================
// 运行时错误
// ============================================================


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
//
// ---- 当前承担了六类职责，后续拆分的目标边界 ----
//   CodeGen          → 已拆到 src/CodeGen.cpp（generateCircuitC + topologicalSort）
//   JitBackend       → 已拆到 src/JitBackend.cpp（LLVM/TCC 分支、符号注入、函数指针解析）
//   CircuitSerializer→ 已拆到 src/CircuitSerializer.cpp（loadCircuitFromJson）
//   NetlistGraph     createNet / connect / disconnect / removeNet / removeComponent
//                    / findNet / check（环路与多驱动检测）/ expandComposites
//                    这一块还没拆：它直接读写 _nets/_components/_bus_nets，
//                    拆出去需要先把这三个容器连带 BitWidth 更新、总线追踪一起搬走。
//   Runtime          init / tick / setWire / getWire / onBusConflict
//                    留在本类是有意的：tick 是最热的一次函数指针调用，不宜再包一层。
// 硬性约束：拆出来的部件必须是编译期组合的具体类型（Circuit 直接持有成员），
// 不许引入虚基类或多态接口——tick/setWire/getWire 在每 tick 的热路径上，
// 后端选择继续用 #ifdef 编译期决定，不能变成运行期分发。
// ============================================================
// 一条总线上的一个三态驱动源：哪个元件的第几个输出。
// 放在命名空间级是因为 CodeGen 需要以只读方式遍历它。
struct BusDriver {
    Component *comp;
    int out_idx;
};

class Circuit {
public:
    Circuit();
    ~Circuit();

    // --- 构建电路 ---
    // 创建线网，位宽由后续连接的引脚自动确定（取最大值）
    Net *createNet(const std::string &name);
    // 添加元件，返回原始指针供后续 connect() 使用。
    // 元件的 prepare()（DLL 加载等）失败时返回 nullptr；error 非空则把原因写进去
    Component *addComponent(std::unique_ptr<Component> comp, CircuitError *error = nullptr);
    // 连接引脚到线网，自动检查多驱动冲突、更新线网位宽。
    // 返回 OK 表示连上；失败时不建立连接，错误码说明原因（引脚不存在 / 多驱动冲突）。
    // 不抛异常：GUI 里用户随手拖线，接错是常态，调用方按返回值处理即可
    CircuitError connect(Component *comp, const std::string &pin_name, Net *net);

    // 断开引脚与线网的连接（已断开时无操作）。输出引脚会自动清理 _bus_nets 三态驱动记录
    void disconnect(Component *comp, const std::string &pin_name);

    // 断开元件的全部引脚（先输出后输入，保证 _bus_nets 正确清理）
    void disconnectAll(Component *comp);

    // 移除元件：断开全部引脚并从电路中销毁。调用后原 Component* 失效。
    // 传进来的指针必须仍在有效期内（不能是已经 removeComponent 掉的野指针），
    // 这一点无法在运行期验证
    void removeComponent(Component *comp);

    // 移除线网：断开所有连接到此线网的引脚，再从电路中移除并销毁
    void removeNet(Net *net);

    // 移除所有线网（断开全部引脚，清空 _nets）
    void removeAllNets();

    // ⚠️ Component 析构前，若其引脚仍连接在线网上，必须先调用 disconnect，
    // 否则 Net 中会残留悬空指针、_bus_nets 中留有脏数据

    // --- 验证与编译 ---
    CircuitError check(std::vector<std::string> *warnings = nullptr); // 环路检测，失败返回错误
    // 校验：展开复合元件 + 环路/拓扑检查，不生成也不编译 JIT。
    // 注意会永久展开并重编号：调用后复合元件消失；若之前编译过，旧 JIT 会先被丢弃
    // （结构已变，不能再用），tick() 在重新 compile() 前直接返回。
    // 拓扑告警只写进 warnings，不直接打印；CLI 层决定怎么呈现
    CircuitError validate(std::vector<std::string> *warnings = nullptr);
    // JIT 编译（内部: 展开复合→检测→生成C→编译→链接）。
    // warnings 非空时收集拓扑告警，由调用方决定怎么呈现；为空时保持老行为打到 stderr
    CircuitError compile(std::vector<std::string> *warnings = nullptr);
    bool isCompiled() const {
        return _compiled;
    }

    // --- 仿真运行 ---
    void init(); // 调用 circuit_init()，清零所有状态
    // 以下 tick 系列在"未编译/编译后结构又变了/有未清错误"时直接返回，
    // 不执行过期的 JIT 代码
    void tick(); // 调用 circuit_tick()，执行一个周期
    // 驱动 clk 线网跑完整时钟周期（低电平拍 + 高电平拍）。需要逐拍控制总线时用 tick()
    // clk_net_id 必须是外部驱动的时钟线（由 setWire/tickCycle 自己写）。如果这根线
    // 同时接着 clkgen 的输出，clkgen 会在每个 tick 的阶段 3 把它覆盖掉，tickCycle
    // 写的电平只能在本拍的组合阶段被看到，时序元件看到的仍是 clkgen 的波形
    void tickCycle(int clk_net_id);
    // 连跑 n 个周期，循环在 JIT 内部完成（省掉每周期一次调用）；约束同 tickCycle
    void tickCycles(int clk_net_id, uint64_t n);
    // 只跑 n 拍、不碰时钟线网：时钟由电路里的 clkgen 自己产生时用这个
    void tickBatch(uint64_t n);
    // JIT 内的死循环，一直跑到进程被 Ctrl+C 终止；未编译/有错误时直接返回
    void tickForever();
    void reset(); // 重置仿真状态（仅清零线网和时序状态，不重建 DLL 实例）
    void deinit(); // 结束仿真（销毁 DLL 状态、调用 deinit 等）
    // 线网槽属于 JIT：未编译或结构已改时读写不做任何事
    void setWire(int id, const std::vector<uint8_t> &value); // 设置线网值
    // 标量快捷路径：高频驱动时钟/复位这类 1 位线网时，不必每拍构造一次 vector
    void setWire(int id, uint64_t value);
    // 直接拿 Net* 驱动，省掉调用方到处写 ->id()。空指针静默忽略，和其他
    // 无效输入（越界 id、未编译）的处理保持一致；findNet() 的返回值可以直接传
    void setWire(Net *net, const std::vector<uint8_t> &value) {
        if (net)
            setWire(net->id(), value);
    }
    void setWire(Net *net, uint64_t value) {
        if (net)
            setWire(net->id(), value);
    }
    // 读取线网值。返回的是 tick 结束时的状态（全部相位跑完之后），
    // 不是组合逻辑的不动点：时序三态输出在阶段 3/4 才更新，读这些总线的
    // 组合元件要到下一拍重算，所以两者在同一个 tick 末尾可能不一致
    std::vector<uint8_t> getWire(int id);

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
    // 按元件 ID 查找。ID 在 compile()/validate() 里按索引重编号，构建期删过元件后
    // 直到下一次重编号前，ID 不保证等于下标
    Component *componentById(int id) const;

    // 本电路展开时实际用到的蓝图名。导出 JSON 时只嵌入这些定义：
    // 同一个 Context 里的库可能还装着别的电路的定义，全导出去会把无关的
    // 文件路径也带进来，导出文件拿到别处就加载不了
    const std::set<std::string> &usedDefinitions() const {
        return _used_definitions;
    }

    // --- 调试 ---
    // 生成等效 C 代码，不触发 JIT 编译。要求网表已展开：先调 validate() 或 compile()
    std::string generateC() const;

    // --- JSON 序列化 ---
    std::string exportJson() const; // 导出电路为 JSON 字符串
    // base_dir 用于解析 definitions 里的相对路径；context 为空时用进程默认上下文
    static std::unique_ptr<Circuit> fromJson(const std::string &json, std::string &error,
                                             const std::string &base_dir = "",
                                             Context *context = nullptr);

    // JSON 元数据：加载时从 $meta 读入、导出时原样写回（都可为空）
    void setMetadata(const std::string &author, const std::string &description) {
        _author = author;
        _description = description;
    }
    [[nodiscard]] const std::string &author() const {
        return _author;
    }
    [[nodiscard]] const std::string &description() const {
        return _description;
    }

    // --- 上下文 ---
    // Context 持有蓝图库（definitions）和 JIT 头文件目录。不显式设置时用进程默认
    // 上下文；需要同时跑多套互不相关的电路时，各自建 Context 再 setContext
    // 必须在添加元件之前调用：复合元件/动态引脚的类型和位宽在 ensurePins 时
    // 已经按旧上下文的蓝图库解析完了，换库会让引脚和展开用的定义对不上
    void setContext(Context &ctx) {
        if (!_components.empty())
            throw std::logic_error("setContext() 必须在添加元件之前调用");
        _context = &ctx;
    }
    Context &context();
    [[nodiscard]] const Context &context() const;

    // --- JIT 头文件搜索路径（等价于操作默认上下文）---
    // 设置后优先使用；为空时使用默认行为（exe 同目录下的 shim/）
    static void setJitIncludeDir(const std::string &dir);
    static const std::string &jitIncludeDir();

private:
    enum class Color { WHITE, GRAY, BLACK };
    std::vector<std::unique_ptr<Net>> _nets;
    std::vector<std::unique_ptr<Component>> _components;
    // expandComposites 成功展开过的定义名（含嵌套展开），导出 JSON 时用。
    // 记录是历史性的：removeComponent 不会回滚，可能多于当前电路实际使用
    std::set<std::string> _used_definitions;

    // 总线追踪：net_id → [(comp, out_idx), ...]（三态输出允许多驱动）
    std::map<int, std::vector<BusDriver>> _bus_nets;
    // JIT 死循环的停止标志：地址通过 JIT 符号表注入，总线冲突/振荡时由
    // onBusConflict 置 1，circuit_tick_forever 每拍检查后返回
    volatile int _stop_flag = 0;

    bool _compiled = false;
    // init() 成功执行过、还没 deinit()：此时 JIT 的静态状态里有 DLL 实例对象，
    // 重新 compile() 前必须先 deinit()，否则 circuit_deinit 被跳过、对象泄漏
    bool _sim_started = false;

    // JSON 元数据（$meta.author / $meta.description），导出时写回
    std::string _author, _description;
    CircuitError _tick_error;

    // JIT 后端是编译期选定的具体类型，不是多态接口——这里的每次调用都只发生在
    // compile() 里，tick 走的是后端解析出来的函数指针，热路径上没有额外间接层
    JitBackend _backend;

    // 编译产物和 Circuit 的入口一一对应。直接持有 JitCompileResult，新增入口时
    // 只改 JitBackend.cpp 的符号解析，这里不用再手工同步一份字段
    JitCompileResult _jit;

    // 显式上下文（可为空，空表示用 Context::global()）
    Context *_context = nullptr;

    Pin *findPin(Component *comp, const std::string &name);
    // 摘掉单个引脚的连接并维护 _bus_nets，不重算位宽（由调用方统一重算一次）
    void detachPin(Component *comp, Pin *pin);
    // 断开/删除后按当前连接重算所有线网位宽（连接时只做增量取最大）
    void recomputeNetWidths();
    void jitCleanup();
    // expandComposites + 重编号 + check，两个入口共用的网表准备步骤
    CircuitError prepareNetlist(std::vector<std::string> *warnings = nullptr);
    // 重新绑定 deinit() 释放掉的外部资源（DLL、介质文件），compile/validate 共用
    CircuitError prepareComponents();
    CircuitError expandComposites();
    bool dfsCycle(int u, const std::vector<std::vector<int>> &adj, std::vector<Color> &color, std::vector<int> &path);
};

} // namespace dsc
