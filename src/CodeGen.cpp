#include "dcs/CodeGen.h"

#include <algorithm>
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

std::string generateCircuitC(void *circuit_ptr, const std::vector<std::unique_ptr<Net>> &nets,
                             const std::vector<std::unique_ptr<Component>> &components,
                             const std::map<int, std::vector<BusDriver>> &bus_nets) {
    std::ostringstream oss;
    oss << R"(#include <dcs_stdint.h>
#include <dcs_stdbool.h>
#include <dcs_util.h>

)";
    oss << std::format("#define NET_COUNT {}\n", nets.size());
    oss << std::format("#define COMP_COUNT {}\n\n", components.size());
    oss << "static uint8_t _w[NET_COUNT][16];\n";
    oss << std::format("static void* _circuit_ptr = (void*)0x{:X};\n", reinterpret_cast<uintptr_t>(circuit_ptr));
    oss << "extern void dcs_bus_conflict(void*, int);\n\n";

    // 每个元件的每个输出：_c_out_<comp_id>_<out_idx>[16] + _c_oe_<comp_id>_<out_idx>
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("static uint8_t _c_out_{}_{}[16];\n", c->id(), oi);
            bool ts = c->outputs()[oi]->isTriState();
            oss << std::format("static bool _c_oe_{}_{} = {};\n", c->id(), oi, ts ? "false" : "true");
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
    oss << "void circuit_tick(void) {\n";
    // 先算出哪些组合元件要重跑：输入直接或间接挂在总线网上的那些。
    // 没有总线网的电路集合为空，阶段2b 就是零开销。
    //
    // 这里只收组合元件是有意的，也是阶段2b 能安全重复执行的前提：
    // 组合元件对输入是纯函数，跑几次结果都一样。反过来，任何带状态的东西
    //（MMIO 设备、有副作用的读）都不允许出现在这个集合里——一个 tick 内被跑多次
    // 会丢掉中断或字节。所以凡是有副作用的元件，必须是时序元件。
    std::set<Component *> bus_affected;
    {
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
    }

    // 组合阶段生成器：执行所有组合元件，并把非总线输出立即传播到 _w
    auto emitCombStage = [&](const std::set<Component *> *only) {
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
                int nb = byte_count(n->bit_width());
                oss << std::format("    if (_c_oe_{}_{}) {{\n", c->id(), oi);
                oss << std::format("        dcs_memcpy(_w[{}], _c_out_{}_{}, {});\n", nid, c->id(), oi, nb);
                oss << std::format("        dcs_memset(_w[{}] + {}, 0, {});\n", nid, nb, 16 - nb);
                // 三态输出关闭时必须把线网清成 0：这里是"执行即传播"，
                // 同一阶段里排在后面的组合元件马上就会读它，读到上一拍的残留值就错了
                oss << "    } else {\n";
                oss << std::format("        dcs_memset(_w[{}], 0, 16);\n", nid);
                oss << "    }\n";
            }
        }
    };

    // 传播阶段生成器：总线决议 + 所有输出的 _c_out → _w
    // track_change 为真时，每写一条总线都比对旧值，把"有没有变化"记到 _bus_changed，
    // 阶段 2b 靠它判断级联是否已经到达不动点
    auto emitPropagation = [&](int round, bool track_change) {
    std::set<int> bus_nets_handled;

    // 总线决议
    for (auto &[net_id, drivers]: bus_nets) {
        int nb = byte_count(nets[net_id]->bit_width());
        oss << "\n    // 总线 " << net_id << "（" << drivers.size() << " 个三态驱动）\n";
        oss << "    {\n";
        // 旧值必须在冲突检测之前取：冲突时下面的 goto 会跳过初���化，
        // 让它之后的比对读到未初始化的栈内存
        if (track_change)
            oss << std::format("        uint8_t _old_{}[16]; dcs_memcpy(_old_{}, _w[{}], 16);\n", net_id, net_id, net_id);
        oss << "        int _active = -1, _conflict = 0;\n";
        for (int di = 0; di < (int) drivers.size(); di++) {
            int cid = drivers[di].comp->id();
            int oi = drivers[di].out_idx;
            if (di == 0)
                oss << std::format("        if (_c_oe_{}_{}) _active = {};\n", cid, oi, di);
            else
                // 多驱动冲突只置标志，不跳转：跳转会绕过下面所有语句（包括旧值快照的初始化）
                oss << std::format(
                        "        if (_c_oe_{}_{}) {{\n"
                        "            if (_active >= 0) {{ dcs_bus_conflict(_circuit_ptr, {}); _conflict = 1; }}\n"
                        "            else _active = {};\n"
                        "        }}\n",
                        cid, oi, net_id, di);
        }
        oss << "        if (!_conflict) {\n";
        oss << "        if (_active >= 0) {\n";
        for (int di = 0; di < (int) drivers.size(); di++) {
            int cid = drivers[di].comp->id();
            int oi = drivers[di].out_idx;
            oss << std::format("            {}if (_active == {}) dcs_memcpy(_w[{}], _c_out_{}_{}, {});\n",
                               di == 0 ? "" : "else ", di, net_id, cid, oi, nb);
        }
        oss << std::format("            dcs_memset(_w[{}] + {}, 0, {});\n", net_id, nb, 16 - nb);
        oss << "        } else {\n";
        oss << std::format("            dcs_memset(_w[{}], 0, 16);  // Hi-Z\n", net_id);
        oss << "        }\n";
        oss << "        }\n";
        if (track_change)
            oss << std::format(
                    "    for (int _i=0;_i<16;_i++) if (_old_{}[_i] != _w[{}][_i]) {{ _bus_changed=1; break; }}\n",
                    net_id, net_id);
        oss << "    }\n";
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
            int nb = byte_count(n->bit_width());
            oss << std::format("    if (_c_oe_{}_{}) {{\n", c->id(), oi);
            oss << std::format("        dcs_memcpy(_w[{}], _c_out_{}_{}, {});\n", nid, c->id(), oi, nb);
            oss << std::format("        dcs_memset(_w[{}] + {}, 0, {});\n", nid, nb, 16 - nb);
            oss << "    } else {\n";
            oss << std::format("        dcs_memset(_w[{}], 0, 16);  // Hi-Z\n", nid);
            oss << "    }\n";
        }
    }
    }; // emitPropagation

    oss << "    // 阶段1: 组合逻辑\n";
    emitCombStage(nullptr);
    if (!bus_affected.empty()) oss << "    int _bus_changed = 0;\n";
    oss << "    // 阶段2: 总线决议 + 时序输出传播\n";
    emitPropagation(0, false);
    // 读总线的组合元件在阶段1 看到的是决议前的值，必须补跑。补跑会改动总线，
    // 又可能影响下一级读总线的元件，所以不能只补一轮——一直迭代到总线不再变化。
    // 上限取元件数，防止电路里出现自激振荡时死循环。
    if (!bus_affected.empty()) {
        oss << "    // 阶段2b: 反复重跑受总线影响的组合元件，直到总线稳定\n";
        oss << "    bool _bus_settled = false;\n";
        oss << std::format("    for (int _bus_round = 0; _bus_round < {}; _bus_round++) {{\n",
                           (int) components.size() + 1);
        oss << "        _bus_changed = 0;\n";
        emitCombStage(&bus_affected);
        emitPropagation(2, true);
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
    oss << "}\n\n";

    oss << R"(void circuit_init(void) {
    dcs_memset(_w, 0, sizeof(_w));
)";
    // 清零所有 _c_out 和 _c_oe
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("    dcs_memset(_c_out_{}_{}, 0, 16);\n", c->id(), oi);
            // 三态使能也要回到初值：否则运行中被置真的三态输出复位后会以 0 驱动总线，
            // 多条三态线挂同一总线时第一拍就可能报假冲突
            oss << std::format("    _c_oe_{}_{} = {};\n", c->id(), oi,
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
    dcs_memset(_w, 0, sizeof(_w));
)";
    // 清零所有 _c_out 和 _c_oe
    for (auto &c: components) {
        for (int oi = 0; oi < (int) c->outputs().size(); oi++) {
            oss << std::format("    dcs_memset(_c_out_{}_{}, 0, 16);\n", c->id(), oi);
            // 三态使能也要回到初值：否则运行中被置真的三态输出复位后会以 0 驱动总线，
            // 多条三态线挂同一总线时第一拍就可能报假冲突
            oss << std::format("    _c_oe_{}_{} = {};\n", c->id(), oi,
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
void circuit_get_wire(int id, uint8_t val[16]) {
    dcs_memcpy(val, _w[id], 16);
}
)";
    return oss.str();
}

} // namespace dsc
