#include "dcs/CodeGen.h"
#include "dcs/CodeGenNames.h"
#include "dcs_shim_embedded.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dsc {

std::vector<Component *> topologicalSort(const std::vector<Component *> &comb) {
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
        // 这里不能用 getCombinationalDeps()：它为三态/时序输出返回空依赖，那是
        // 环路检测的语义（三态可断开，不能据此判环）。排序是数据流顺序，三态门
        // 仍必须排在驱动它输入的组合元件之后，否则它读到的是上一拍的输入
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

namespace {

// 输入直接或间接挂在总线网上的组合元件：阶段 2b 要重跑它们。
// 只收组合元件是阶段 2b 能安全重复执行的前提：组合元件对输入是纯函数，
// 跑几次结果都一样；带状态的元件（设备、有副作用的读）绝不能进这个集合
std::set<Component *> computeBusAffected(const std::vector<Component *> &sorted,
                                         const std::map<int, std::vector<BusDriver>> &bus_nets) {
    std::set<Component *> bus_affected;
    std::vector<Component *> queue;
    for (auto *c: sorted) {
        if (!c->hasCombinational())
            continue;
        for (auto &pin: c->inputs())
            if (pin->net() && bus_nets.count(pin->net()->id())) {
                if (bus_affected.insert(c).second)
                    queue.push_back(c);
                break;
            }
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        for (auto &op: queue[qi]->outputs()) {
            if (!op->net())
                continue;
            for (auto *down: sorted) {
                if (!down->hasCombinational() || bus_affected.count(down))
                    continue;
                for (auto &pin: down->inputs())
                    if (pin->net() == op->net()) {
                        bus_affected.insert(down);
                        queue.push_back(down);
                        break;
                    }
            }
        }
    }
    return bus_affected;
}

// 组合阶段：执行组合元件，并把非总线三态输出立即传播到 _w
void emitCombStage(std::ostream &oss, const std::vector<Component *> &sorted,
                   const std::set<Component *> *only,
                   const std::map<int, std::vector<BusDriver>> &bus_nets) {
    for (auto *c: sorted) {
        if (only && !only->count(c))
            continue;
        if (!c->hasCombinational())
            continue;
        oss << "    " << c->funcName_comb() << "();\n";
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            Net *n = c->outputs()[oi]->net();
            if (!n)
                continue;
            int nid = n->id();
            if (bus_nets.count(nid))
                continue; // 总线网留给决议阶段
            // 非三态输出在元件函数里已经由 genOutputWrite 直接写进 _w 了，
            // 这里再整块拷一遍是纯冗余，每拍每个输出能省一次 16 字节写
            if (!c->outputs()[oi]->isTriState())
                continue;
            oss << std::format("    if ({}) {{\n", cgen::cOe(c->id(), oi));
            // _c_out 的高位由 genOutputWrite 清零，整块 16 字节一次拷过去即可：
            // 比"拷 nb 字节再清剩余"少一次调用，定长拷贝也更容易被编译器展开
            oss << std::format("        dcs_memcpy({}, {}, 16);\n", cgen::wire(nid), cgen::cOut(c->id(), oi));
            // 三态输出关闭时必须把线网清成 0：这里是"执行即传播"，
            // 同一阶段里排在后面的组合元件马上就会读它，读到上一拍的残留值就错了
            oss << "    } else {\n";
            oss << std::format("        dcs_memset({}, 0, 16);\n", cgen::wire(nid));
            oss << "    }\n";
        }
    }
}

// 单条总线的决议：多驱动冲突检测 + 把选中驱动源的 _c_out 拷到 _w。
// track_change 为真时比对旧值，把"有没有变化"记到 _bus_changed（阶段 2b 用）
void emitBusResolution(std::ostream &oss, int net_id, const std::vector<BusDriver> &drivers, bool track_change) {
    oss << "\n    // 总线 " << net_id << "（" << drivers.size() << " 个三态驱动）\n";
    oss << "    {\n";
    // 旧值必须在冲突检测之前取：冲突时下面的分支会跳过初始化，
    // 让它之后的比对读到未初始化的栈内存
    if (track_change)
        oss << std::format("        uint8_t _old_{}[16]; dcs_memcpy(_old_{}, {}, 16);\n", net_id, net_id,
                           cgen::wire(net_id));
    oss << "        int _active = -1, _conflict = 0;\n";
    for (int di = 0; di < (int) drivers.size(); di++) {
        int cid = drivers[di].comp->id();
        int oi = drivers[di].out_idx;
        if (di == 0)
            oss << std::format("        if ({}) _active = {};\n", cgen::cOe(cid, oi), di);
        else
            // 多驱动冲突只置标志，不跳转：跳转会绕过下面所有语句（包括旧值快照的初始化）
            oss << std::format(
                    "        if ({}) {{\n"
                    "            if (_active >= 0) {{ dcs_bus_conflict(_circuit_ptr, {}); _conflict = 1; }}\n"
                    "            else _active = {};\n"
                    "        }}\n",
                    cgen::cOe(cid, oi), net_id, di);
    }
    oss << "        if (!_conflict) {\n";
    oss << "        if (_active >= 0) {\n";
    for (int di = 0; di < (int) drivers.size(); di++) {
        int cid = drivers[di].comp->id();
        int oi = drivers[di].out_idx;
        oss << std::format("            {}if (_active == {}) dcs_memcpy({}, {}, 16);\n", di == 0 ? "" : "else ",
                           di, cgen::wire(net_id), cgen::cOut(cid, oi));
    }
    oss << "        } else {\n";
    oss << std::format("            dcs_memset({}, 0, 16);  // Hi-Z\n", cgen::wire(net_id));
    oss << "        }\n";
    oss << "        }\n";
    if (track_change)
        oss << std::format(
                "    for (int _i=0;_i<16;_i++) if (_old_{}[_i] != {}[_i]) {{ _bus_changed=1; break; }}\n",
                net_id, cgen::wire(net_id));
    oss << "    }\n";
}

// 总线决议 + 所有输出的 _c_out → _w。
// track_change 为真时，每写一条总线都比对旧值，把"有没有变化"记到 _bus_changed，
// 阶段 2b 靠它判断级联是否已经到达不动点
void emitPropagation(std::ostream &oss, const std::vector<std::unique_ptr<Component>> &components,
                     const std::map<int, std::vector<BusDriver>> &bus_nets, bool track_change) {
    std::set<int> bus_nets_handled;

    for (auto &[net_id, drivers]: bus_nets) {
        emitBusResolution(oss, net_id, drivers, track_change);
        bus_nets_handled.insert(net_id);
    }

    // 非总线线网：若驱动活跃则拷贝 _c_out → _w
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            Net *n = c->outputs()[oi]->net();
            if (!n)
                continue;
            int nid = n->id();
            if (bus_nets_handled.count(nid))
                continue; // 总线已处理
            // 非三态输出已经由 genOutputWrite 直接写进 _w，这里再拷一遍是纯冗余
            if (!c->outputs()[oi]->isTriState())
                continue;
            oss << std::format("    if ({}) {{\n", cgen::cOe(c->id(), oi));
            oss << std::format("        dcs_memcpy({}, {}, 16);\n", cgen::wire(nid), cgen::cOut(c->id(), oi));
            oss << "    } else {\n";
            oss << std::format("        dcs_memset({}, 0, 16);  // Hi-Z\n", cgen::wire(nid));
            oss << "    }\n";
        }
    }
}

// 阶段 3 之后：时序元件此时才写 _c_out/_c_oe，必须在同一拍内传播到 _w，
// 否则下一拍阶段 1 读到的是再上一拍的值（组合驱动的总线在阶段 2 已决议过）。
// 只处理"有时序驱动的总线"和非总线上的时序三态输出：没有这些的电路
// 生成阶段 4 为空，不给热路径加任何开销
void emitSeqTriPropagation(std::ostream &oss, const std::vector<std::unique_ptr<Component>> &components,
                           const std::map<int, std::vector<BusDriver>> &bus_nets) {
    std::set<int> buses_done;
    for (auto &[net_id, drivers]: bus_nets) {
        bool seq_driven = false;
        for (auto &d: drivers)
            if (d.comp->hasSequential()) {
                seq_driven = true;
                break;
            }
        if (!seq_driven)
            continue;
        emitBusResolution(oss, net_id, drivers, false);
        buses_done.insert(net_id);
    }
    for (auto &c: components) {
        if (!c->hasSequential())
            continue;
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            Net *n = c->outputs()[oi]->net();
            if (!n || !c->outputs()[oi]->isTriState() || buses_done.count(n->id()))
                continue;
            oss << std::format("    if ({}) {{\n", cgen::cOe(c->id(), oi));
            oss << std::format("        dcs_memcpy({}, {}, 16);\n", cgen::wire(n->id()), cgen::cOut(c->id(), oi));
            oss << "    } else {\n";
            oss << std::format("        dcs_memset({}, 0, 16);  // Hi-Z\n", cgen::wire(n->id()));
            oss << "    }\n";
        }
    }
}

} // namespace

std::string generateCircuitC(void *circuit_ptr, volatile int *stop_flag_ptr,
                             const std::vector<std::unique_ptr<Net>> &nets,
                             const std::vector<std::unique_ptr<Component>> &components,
                             const std::map<int, std::vector<BusDriver>> &bus_nets) {
    // 生成代码把 id 直接当下标用（_c_out_<id>、_w[<id>]），前提是
    // prepareNetlist 已经把 id 重编号成连续索引。这个约定跨 Circuit/CodeGen，
    // 在这里断言比在运行期读到错位数组好
    for (size_t i = 0; i < components.size(); i++)
        assert(components[i]->id() == (int) i);
    for (size_t i = 0; i < nets.size(); i++)
        assert(nets[i]->id() == (int) i);

    std::ostringstream oss;
    // shim 内容由 CMake 从 shim/*.h 生成并内嵌，运行时不需要 include 目录
    oss << dsc::kShimSource << "\n";
    oss << std::format("#define NET_COUNT {}\n", nets.size());
    oss << std::format("#define COMP_COUNT {}\n\n", components.size());
    // 未连接的输入引脚 netId() 返回 -1，元件生成的代码会直接读 _w[-1]。在线网数组
    // 前面留一个恒零槽位，_w[-1] 落在这个槽位里，既保住悬空输入读 0 的语义，又不用
    // 改上百处 _w[nid] 的生成代码。只有输入会读到负下标：输出没接线时 CodeGen 直接跳过
    oss << "static uint8_t _w_all[NET_COUNT + 1][16];\n";
    oss << "#define _w (_w_all + 1)\n";
    oss << std::format("static void* _circuit_ptr = (void*)0x{:X};\n", reinterpret_cast<uintptr_t>(circuit_ptr));
    oss << "extern void dcs_bus_conflict(void*, int);\n";
    // tickForever 的停止标志：地址直接编进代码，元件状态指针也是这个做法
    oss << std::format("static volatile int *_dcs_stop_flag = (volatile int*)0x{:X};\n\n",
                       reinterpret_cast<uintptr_t>(stop_flag_ptr));

    // 每个元件的每个输出：_c_out_<comp_id>_<out_idx>[16] + _c_oe_<comp_id>_<out_idx>
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("static uint8_t {}[16];\n", cgen::cOut(c->id(), oi));
            bool ts = c->outputs()[oi]->isTriState();
            oss << std::format("static bool {} = {};\n", cgen::cOe(c->id(), oi), ts ? "false" : "true");
        }
    }
    oss << "\n";

    std::vector<Component *> comb, seq;
    for (auto &c: components) {
        if (c->hasCombinational())
            comb.push_back(c.get());
        if (c->hasSequential())
            seq.push_back(c.get());
    }

    for (auto &c: components) {
        auto def = c->genStructDef();
        if (!def.empty())
            oss << def << "\n\n";
    }
    for (auto &c: components) {
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
    for (auto &c: components) {
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
    for (auto &c: components) {
        auto *s = c.get();
        if (s->hasSequential())
            oss << s->genFuncDef_seq() << "\n";
    }

    // === circuit_tick ===
    oss << std::format("void {}(void) {{\n", cgen::kTickFn);
    // 先算出哪些组合元件要重跑：输入直接或间接挂在总线网上的那些。
    // 没有总线网的电路集合为空，阶段2b 就是零开销
    std::set<Component *> bus_affected = computeBusAffected(sorted, bus_nets);

    oss << "    // 阶段1: 组合逻辑\n";
    emitCombStage(oss, sorted, nullptr, bus_nets);
    if (!bus_affected.empty()) oss << "    int _bus_changed = 0;\n";
    oss << "    // 阶段2: 总线决议 + 时序输出传播\n";
    emitPropagation(oss, components, bus_nets, false);
    // 读总线的组合元件在阶段1 看到的是决议前的值，必须补跑。补跑会改动总线，
    // 又可能影响下一级读总线的元件，所以不能只补一轮——一直迭代到总线不再变化。
    // 上限取元件数，防止电路里出现自激振荡时死循环。
    if (!bus_affected.empty()) {
        oss << "    // 阶段2b: 反复重跑受总线影响的组合元件，直到总线稳定\n";
        oss << "    bool _bus_settled = false;\n";
        oss << std::format("    for (int _bus_round = 0; _bus_round < {}; _bus_round++) {{\n",
                           (int) components.size() + 1);
        oss << "        _bus_changed = 0;\n";
        emitCombStage(oss, sorted, &bus_affected, bus_nets);
        emitPropagation(oss, components, bus_nets, true);
        oss << "        if (!_bus_changed) { _bus_settled = true; break; }\n";
        oss << "    }\n";
        oss << "    if (!_bus_settled) dcs_bus_conflict(_circuit_ptr, -1);\n";
    }

    oss << "    // 阶段3: 时序逻辑\n";
    for (auto &c: components) {
        auto *s = c.get();
        if (s->hasSequential())
            oss << "    " << s->funcName_seq() << "();\n";
    }
    oss << "    // 阶段4: 时序三态输出传播（本拍写、本拍可见）\n";
    emitSeqTriPropagation(oss, components, bus_nets);
    oss << "}\n\n";

    // 时钟线网只有 1 位，直接写第一个字节即可；其余字节在 init 时清零且无人写过。
    // 连续跑多个周期时循环留在 JIT 里，省掉每周期一次跨模块调用
    oss << R"(void circuit_tick_cycle(int clk_id) {
    _w[clk_id][0] = 0;
    circuit_tick();
    _w[clk_id][0] = 1;
    circuit_tick();
}
void circuit_tick_cycles(int clk_id, uint64_t n) {
    for (uint64_t _i = 0; _i < n; _i++) {
        _w[clk_id][0] = 0;
        circuit_tick();
        _w[clk_id][0] = 1;
        circuit_tick();
    }
}
// 只跑拍、不碰时钟线网：时钟由电路里的 clkgen 自己产生的场合用这个。
// 循环留在 JIT 里，长时间仿真时省掉每拍一次跨模块调用
void circuit_tick_batch(uint64_t n) {
    for (uint64_t _i = 0; _i < n; _i++) circuit_tick();
}
// 死循环放在 JIT 里：C++ 不再每拍跨模块调用，--forever 模式一直跑，Ctrl+C 退出。
// 运行期出现总线冲突/振荡时 onBusConflict 置停止标志，这里必须返回，
// 否则调用方永远无法观察到错误
void circuit_tick_forever(void) {
    for (;;) { circuit_tick(); if (*_dcs_stop_flag) return; }
}
// 一直跑下去不返回：C++ 侧调用一次就停在这里，靠 Ctrl+C 结束进程。
// 循环体在 JIT 里，没有每拍跨模块调用的开销
void circuit_run_forever(void) {
    for (;;) circuit_tick();
}

void circuit_init(void) {
    dcs_memset(_w_all, 0, sizeof(_w_all));
)";
    // 清零所有 _c_out 和 _c_oe
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("    dcs_memset({}, 0, 16);\n", cgen::cOut(c->id(), oi));
            // 三态使能也要回到初值：否则运行中被置真的三态输出复位后会以 0 驱动总线，
            // 多条三态线挂同一总线时第一拍就可能报假冲突
            oss << std::format("    {} = {};\n", cgen::cOe(c->id(), oi),
                               c->outputs()[oi]->isTriState() ? "false" : "true");
        }
    }
    for (auto &c: components) {
        auto init = c->genInitCode();
        if (!init.empty())
            oss << init << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_deinit(void) {
)";
    for (auto &c: components) {
        auto deinit = c->genDeinitCode();
        if (!deinit.empty())
            oss << deinit << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_reset(void) {
    dcs_memset(_w_all, 0, sizeof(_w_all));
)";
    // 清零所有 _c_out 和 _c_oe
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("    dcs_memset({}, 0, 16);\n", cgen::cOut(c->id(), oi));
            // 三态使能也要回到初值：否则运行中被置真的三态输出复位后会以 0 驱动总线，
            // 多条三态线挂同一总线时第一拍就可能报假冲突
            oss << std::format("    {} = {};\n", cgen::cOe(c->id(), oi),
                               c->outputs()[oi]->isTriState() ? "false" : "true");
        }
    }
    for (auto &c: components) {
        auto reset = c->genResetCode();
        if (!reset.empty())
            oss << reset << "\n";
    }
    oss << "}\n\n";

    oss << R"(void circuit_set_wire(int id, const uint8_t val[16]) {
    dcs_memcpy(_w[id], val, 16);
}
void circuit_set_wire_u64(int id, unsigned long long v) {
    dcs_memcpy(_w[id], &v, 8);
    dcs_memset(_w[id] + 8, 0, 8);
}
void circuit_get_wire(int id, uint8_t val[16]) {
    dcs_memcpy(val, _w[id], 16);
}
)";
    return oss.str();
}

} // namespace dsc
