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
#include <stdexcept>
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

// 输入直接或间接挂在三态驱动线网上的组合元件：阶段 2b 要重跑它们。
// 必须按"任何三态驱动"来找，而不是只看 _bus_nets：后者只登记多驱动共线的线网，
// 单驱动三态线网由 emitCombStage 直接传播，一个常使能的三态门照样能把组合环
// 悄悄接回来（check() 不为三态输出建边），只认 _bus_nets 就没人兜底。
// 只收组合元件是阶段 2b 能安全重复执行的前提：组合元件对输入是纯函数，
// 跑几次结果都一样；带状态的元件（设备、有副作用的读）绝不能进这个集合。
// BFS 也只沿组合元件建边：时序输出在阶段 2b 期间不会变（阶段 3 才算），
// 所以"三态线网 → 时序元件 → 下游组合元件"这条路径不需要重跑下游。
// 将来如果引入阶段 2b 内就会变的时序输出，这里的边要重新审视。
std::set<Component *> computeTriAffected(const std::vector<Component *> &sorted,
                                         const std::vector<std::unique_ptr<Component>> &components) {
    std::set<int> tri_nets;
    for (auto &c: components)
        for (auto &p: c->outputs())
            if (p->isTriState() && p->net())
                tri_nets.insert(p->net()->id());
    std::set<Component *> tri_affected;
    std::vector<Component *> queue;
    for (auto *c: sorted) {
        if (!c->hasCombinational())
            continue;
        for (auto &pin: c->inputs())
            if (pin->net() && tri_nets.count(pin->net()->id())) {
                if (tri_affected.insert(c).second)
                    queue.push_back(c);
                break;
            }
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        for (auto &op: queue[qi]->outputs()) {
            if (!op->net())
                continue;
            for (auto *down: sorted) {
                if (!down->hasCombinational() || tri_affected.count(down))
                    continue;
                for (auto &pin: down->inputs())
                    if (pin->net() == op->net()) {
                        tri_affected.insert(down);
                        queue.push_back(down);
                        break;
                    }
            }
        }
    }
    return tri_affected;
}

// 组合阶段：执行组合元件，并把非总线三态输出立即传播到 _w。
// track_change 为真时顺带记录三态线网有没有变化（阶段 2b 的收敛判据）
void emitCombStage(std::ostream &oss, const std::vector<Component *> &sorted,
                   const std::set<Component *> *only,
                   const std::map<int, std::vector<BusDriver>> &bus_nets, bool track_change) {
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
            // 单驱动三态线网的传播就发生在这一句里，阶段 2b 判断"这轮补跑有没有
            // 改变线网"只能在这里取旧值；emitPropagation 的比对只覆盖 _bus_nets
            if (track_change)
                oss << std::format("    uint8_t _old_{}_{}[16]; dcs_memcpy(_old_{}_{}, {}, 16);\n", c->id(), oi,
                                   c->id(), oi, cgen::wire(nid));
            oss << std::format("    if ({}) {{\n", cgen::cOe(c->id(), oi));
            // _c_out 的高位由 genOutputWrite 清零，整块 16 字节一次拷过去即可：
            // 比"拷 nb 字节再清剩余"少一次调用，定长拷贝也更容易被编译器展开
            oss << std::format("        dcs_memcpy({}, {}, 16);\n", cgen::wire(nid), cgen::cOut(c->id(), oi));
            // 三态输出关闭时必须把线网清成 0：这里是"执行即传播"，
            // 同一阶段里排在后面的组合元件马上就会读它，读到上一拍的残留值就错了
            oss << "    } else {\n";
            oss << std::format("        dcs_memset({}, 0, 16);\n", cgen::wire(nid));
            oss << "    }\n";
            if (track_change)
                oss << std::format(
                        "    for (int _i=0;_i<16;_i++) if (_old_{}_{}[_i] != {}[_i]) {{ _bus_changed=1; break; }}\n",
                        c->id(), oi, cgen::wire(nid));
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

    // 非总线线网：若驱动活跃则拷贝 _c_out → _w。
    // 这里不区分 comb/seq：时序三态输出会用上一拍留在 _c_out/_c_oe 的值再写一次
    // _w，阶段 3 算出本拍值后由阶段 4（emitSeqTriPropagation）覆盖，语义正确。
    // 阶段 4 不是冗余，不要因为"阶段 2 已经拷过"就删掉它
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

std::string generateCircuitC(void *circuit_ptr, const void *stop_flag_ptr, std::uint64_t tick_ns,
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

    // clkgen 可以在输出上声明伪时钟线：每 tick 都算某个方向的沿，不做比较
    for (auto &n: nets)
        n->setPseudoEdge(PinEdge::None);
    for (auto &c: components) {
        for (auto &p: c->outputs()) {
            const PinEdge de = p->driveEdge();
            if (de == PinEdge::None)
                continue;
            if (de != PinEdge::Rising && de != PinEdge::Falling)
                throw std::logic_error("伪时钟线只能是 Rising 或 Falling");
            if (!p->net() || p->bit_width() != 1)
                throw std::logic_error(std::format("元件 {} 的伪时钟输出 {} 必须是已连接的 1 位线网",
                                                   c->name(), p->name()));
            p->net()->setPseudoEdge(de);
        }
    }

    // 收集所有声明了 PinEdge 的输入线网。同一条时钟线被多个元件共享时只算一次；
    // 一个元件要上升沿、另一个要下降沿时合并成两者都算
    struct EdgeNet {
        int net_id = -1;
        int width = 1;
        bool rise = false, fall = false, change = false;
        int change_slot = -1;
    };
    std::map<int, EdgeNet> edge_nets;
    for (auto &c: components) {
        for (auto &p: c->inputs()) {
            if (p->edge() == PinEdge::None)
                continue;
            const int nid = p->netId();
            if (nid < 0)
                continue; // 悬空引脚没有线网可检测
            if (const PinEdge pseudo = p->net()->pseudoEdge(); pseudo != PinEdge::None) {
                // 伪时钟线只有"每 tick 一个沿"的标志，没有电平变化。DLL/Verilator
                // 模型靠真实电平做内部边沿检测，接上来会永远看不到沿，直接报错
                if (p->edge() != pseudo)
                    throw std::logic_error(std::format(
                            "元件 {} 的引脚 {} 接了每 tick {} 的伪时钟线，但引脚没有声明同名 PinEdge；"
                            "DLL/Verilator 元件必须接真实 clkgen",
                            c->name(), p->name(), static_cast<int>(pseudo)));
                continue; // 伪时钟线不需要边沿比较
            }
            EdgeNet &e = edge_nets[nid];
            e.net_id = nid;
            // 按引脚位宽判断，不按线网位宽：1 位引脚接在宽线网上仍然只取 bit0
            e.width = std::max(e.width, p->bit_width());
            if ((p->edge() == PinEdge::Rising || p->edge() == PinEdge::Falling) && p->bit_width() != 1)
                throw std::logic_error(std::format("元件 {} 的引脚 {} 是 {} 位，不能声明 Rising/Falling；"
                                                   "多位信号请用 PinEdge::Change",
                                                   c->name(), p->name(), p->bit_width()));
            if (p->edge() == PinEdge::Rising)
                e.rise = true;
            else if (p->edge() == PinEdge::Falling)
                e.fall = true;
            else if (p->edge() == PinEdge::Change)
                e.change = true;
            if (e.width > 1 && (e.rise || e.fall))
                throw std::logic_error(
                        std::format("线网 {} 上同时有 1 位的上升/下降沿引脚和多位变化引脚，声明冲突", nid));
        }
    }
    int change_slots = 0;
    bool has_single_bit_edge = false;
    for (auto &[nid, e]: edge_nets) {
        if (e.width == 1)
            has_single_bit_edge = true;
        else if (e.change)
            e.change_slot = change_slots++;
    }

    std::ostringstream oss;
    // shim 内容由 CMake 从 shim/*.h 生成并内嵌，运行时不需要 include 目录
    oss << dsc::kShimSource << "\n";
    oss << std::format("#define NET_COUNT {}\n", nets.size());
    oss << std::format("#define COMP_COUNT {}\n", components.size());
    // 一个仿真 tick 对应多少 ns；元件的物理时间换算在生成期完成，运行期不数 tick
    oss << std::format("#define DCS_TICK_NS {}\n\n", tick_ns);
    // 未连接的输入引脚 netId() 返回 -1，元件生成的代码会直接读 _w[-1]。在线网数组
    // 前面留一个恒零槽位，_w[-1] 落在这个槽位里，既保住悬空输入读 0 的语义，又不用
    // 改上百处 _w[nid] 的生成代码。只有输入会读到负下标：输出没接线时 CodeGen 直接跳过
    oss << "static uint8_t _w_all[NET_COUNT + 1][16];\n";
    oss << "#define _w (_w_all + 1)\n";
    oss << std::format("static void* _circuit_ptr = (void*)0x{:X};\n", reinterpret_cast<uintptr_t>(circuit_ptr));
    oss << "extern void dcs_bus_conflict(void*, int);\n";
    // tickForever 的停止标志：地址直接编进代码，元件状态指针也是这个做法
    oss << std::format("static const void *_dcs_stop_flag = (const void*)0x{:X};\n\n",
                       reinterpret_cast<uintptr_t>(stop_flag_ptr));

    // 统一边沿检测状态：只覆盖声明过 PinEdge 的线网。多位 Change 用快照比较，
    // 1 位线网用上拍值；flags 的 bit0/1/2 分别是上升/下降/变化
    if (!edge_nets.empty()) {
        oss << "static uint8_t _dcs_edge_flags[NET_COUNT];\n";
        if (has_single_bit_edge)
            oss << "static uint8_t _dcs_edge_prev[NET_COUNT];\n";
        if (change_slots > 0)
            oss << std::format("static uint8_t _dcs_change_prev[{}][16];\n", change_slots);
    }

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
    // 先算出哪些组合元件要重跑：输入直接或间接挂在三态驱动线网上的那些
    // （包括单驱动三态线网，见 computeTriAffected 的说明）。
    // 电路里没有三态元件时集合为空，阶段2b 就是零开销
    std::set<Component *> tri_affected = computeTriAffected(sorted, components);

    // 组合阶段读到的时钟线网是上一拍末的值：clkgen 是时序元件，输出要到阶段 3
    // 才写回 _w。组合元件不应该自己读时钟做边沿检测（那是时序元件的职责），
    // 真需要当前拍的时钟值只能用时序元件
    oss << "    // 阶段1: 组合逻辑\n";
    emitCombStage(oss, sorted, nullptr, bus_nets, false);
    if (!tri_affected.empty()) oss << "    int _bus_changed = 0;\n";
    oss << "    // 阶段2: 总线决议 + 时序输出传播\n";
    emitPropagation(oss, components, bus_nets, false);
    // 冲突在阶段 2 就已经确定，继续跑阶段 3 会让时序状态基于冲突后的线网推进，
    // 出错后 dump 出来像是有一堆无关的错乱。只有存在三态总线的电路才可能冲突，
    // 其它电路不加这次 volatile 读，热路径没有额外开销
    if (!bus_nets.empty())
        oss << "    if (dcs_load_acq(_dcs_stop_flag)) return;\n";
    // 读总线的组合元件在阶段1 看到的是决议前的值，必须补跑。补跑会改动总线，
    // 又可能影响下一级读总线的元件，所以不能只补一轮——一直迭代到总线不再变化。
    // 上限取元件数，防止电路里出现自激振荡时死循环。
    if (!tri_affected.empty()) {
        oss << "    // 阶段2b: 反复重跑受三态线网影响的组合元件，直到线网稳定\n";
        oss << "    bool _bus_settled = false;\n";
        oss << std::format("    for (int _bus_round = 0; _bus_round < {}; _bus_round++) {{\n",
                           (int) components.size() + 1);
        oss << "        _bus_changed = 0;\n";
        emitCombStage(oss, sorted, &tri_affected, bus_nets, true);
        emitPropagation(oss, components, bus_nets, true);
        oss << "        if (!_bus_changed) { _bus_settled = true; break; }\n";
        oss << "    }\n";
        oss << "    if (!_bus_settled) dcs_bus_conflict(_circuit_ptr, -1);\n";
        // 振荡信号在这一段里产生。没有多驱动总线的电路也会因为单驱动三态环
        // 走到这里，所以这句不能跟上面的检查合并成一次
        oss << "    if (dcs_load_acq(_dcs_stop_flag)) return;\n";
    }

    // 边沿必须在阶段 2 之后算：clkgen 是时序元件，它的输出要等阶段 2 才写进
    // _w，阶段 3 的时序元件读到的就是这里的值。放在 tick 最开头会晚一拍
    if (!edge_nets.empty()) {
        oss << "    // 统一边沿检测（只更新声明过 PinEdge 的线网）\n";
        for (auto &[nid, e]: edge_nets) {
            if (e.width == 1) {
                oss << std::format(
                        "    {{ const uint8_t _c = _w[{}][0] & 1;"
                        " _dcs_edge_flags[{}] = (uint8_t)((_c != _dcs_edge_prev[{}]) ? ((_c ? 1u : 2u) | 4u) : 0u);"
                        " _dcs_edge_prev[{}] = _c; }}\n",
                        nid, nid, nid, nid);
            } else {
                const int bytes = std::min(16, (e.width + 7) / 8);
                oss << std::format(
                        "    {{ if (dcs_memcmp(_w[{}], _dcs_change_prev[{}], {}u) != 0) {{"
                        " dcs_memcpy(_dcs_change_prev[{}], _w[{}], {}u); _dcs_edge_flags[{}] = 4u; }}"
                        " else _dcs_edge_flags[{}] = 0u; }}\n",
                        nid, e.change_slot, bytes, e.change_slot, nid, bytes, nid, nid);
            }
        }
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
// 批量入口必须检查停止标志：onBusConflict 置位后继续跑，会在已经出错的状态下
// 再推进几万拍，时序状态和输出都被污染。每 256 拍查一次而不是每拍查——
// volatile 读会打断循环优化，整机仿真里每个 tick 都加一次读不划算，
// 出错的代价只是多跑最多 255 拍
void circuit_tick_cycles(int clk_id, uint64_t n) {
    for (uint64_t _i = 0; _i < n; _i++) {
        _w[clk_id][0] = 0;
        circuit_tick();
        _w[clk_id][0] = 1;
        circuit_tick();
        if ((_i & 15u) == 15u && dcs_load_acq(_dcs_stop_flag)) return;
    }
}
// 只跑拍、不碰时钟线网：时钟由电路里的 clkgen 自己产生的场合用这个。
// 循环留在 JIT 里，长时间仿真时省掉每拍一次跨模块调用
void circuit_tick_batch(uint64_t n) {
    for (uint64_t _i = 0; _i < n; _i++) {
        circuit_tick();
        if ((_i & 255u) == 255u && dcs_load_acq(_dcs_stop_flag)) return;
    }
}
// 死循环放在 JIT 里：C++ 不再每拍跨模块调用，--forever 模式一直跑，Ctrl+C 退出。
// 运行期出现总线冲突/振荡时 onBusConflict 置停止标志，这里必须返回，
// 否则调用方永远无法观察到错误。
// 停止标志每 16 拍查一次：LLVM 后端把 acquire 读编成普通 mov，但 TCC 后端
// dcs_load_acq 是一次函数调用，每拍都查会拖慢小电路；多等 15 拍没人感知
void circuit_tick_forever(void) {
    for (uint32_t _i = 0;; _i++) {
        circuit_tick();
        if ((_i & 15u) == 15u && dcs_load_acq(_dcs_stop_flag)) return;
    }
}

void circuit_init(void) {
    dcs_memset(_w_all, 0, sizeof(_w_all));
)";
    if (!edge_nets.empty()) {
        oss << "    dcs_memset(_dcs_edge_flags, 0, sizeof(_dcs_edge_flags));\n";
        if (has_single_bit_edge)
            oss << "    dcs_memset(_dcs_edge_prev, 0, sizeof(_dcs_edge_prev));\n";
        if (change_slots > 0)
            oss << "    dcs_memset(_dcs_change_prev, 0, sizeof(_dcs_change_prev));\n";
    }
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
    if (!edge_nets.empty()) {
        oss << "    dcs_memset(_dcs_edge_flags, 0, sizeof(_dcs_edge_flags));\n";
        if (has_single_bit_edge)
            oss << "    dcs_memset(_dcs_edge_prev, 0, sizeof(_dcs_edge_prev));\n";
        if (change_slots > 0)
            oss << "    dcs_memset(_dcs_change_prev, 0, sizeof(_dcs_change_prev));\n";
    }
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
