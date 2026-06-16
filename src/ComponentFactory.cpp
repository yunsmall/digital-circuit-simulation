// ComponentFactory — 所有元件类型的集中注册
//
// 所有工厂注册集中在此文件，由全局静态对象在 main 前完成。
// ComponentFactory 自身被各处引用，此 .obj 必定被链接，无需 /WHOLEARCHIVE。
#include "dcs/ComponentFactory.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/components/Arithmetic.h"
#include "dcs/components/Composite.h"
#include "dcs/components/FloatPoint.h"
#include "dcs/components/Dll.h"
#include "dcs/components/Gates.h"
#include "dcs/components/Memory.h"
#include "dcs/components/Misc.h"
#include "dcs/components/Print.h"
#include "dcs/components/Sequential.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace dsc {

ComponentFactory &ComponentFactory::instance() {
    static ComponentFactory f;
    return f;
}

} // namespace dsc

namespace {

using Params = std::unordered_map<std::string, std::string>;

// 整型参数解析
static int _gi(const Params &p, const std::string &k, int d) {
    auto it = p.find(k);
    return it != p.end() ? std::stoi(it->second) : d;
}

struct InitAllComponents {
    InitAllComponents() {
        auto &f = dsc::ComponentFactory::instance();
        using C = std::unique_ptr<dsc::Component>;

        // ============================================================
        // 门电路
        // ============================================================
        auto mk = [](const std::string &n, const Params &p, auto ctor) {
            return ctor(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8));
        };
        f.registerType({"and", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}},
                       [mk](const std::string &n, const Params &p) -> C {
                           return mk(n, p, [](auto &&...a) { return std::make_unique<dsc::GateAND>(a...); });
                       });
        f.registerType({"or", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}},
                       [mk](const std::string &n, const Params &p) -> C {
                           return mk(n, p, [](auto &&...a) { return std::make_unique<dsc::GateOR>(a...); });
                       });
        f.registerType({"nand", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}},
                       [mk](const std::string &n, const Params &p) -> C {
                           return mk(n, p, [](auto &&...a) { return std::make_unique<dsc::GateNAND>(a...); });
                       });
        f.registerType({"nor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}},
                       [mk](const std::string &n, const Params &p) -> C {
                           return mk(n, p, [](auto &&...a) { return std::make_unique<dsc::GateNOR>(a...); });
                       });
        f.registerType({"xor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}},
                       [mk](const std::string &n, const Params &p) -> C {
                           return mk(n, p, [](auto &&...a) { return std::make_unique<dsc::GateXOR>(a...); });
                       });
        f.registerType({"xnor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}},
                       [mk](const std::string &n, const Params &p) -> C {
                           return mk(n, p, [](auto &&...a) { return std::make_unique<dsc::GateXNOR>(a...); });
                       });
        f.registerType({"not", {{"bit_width", "8"}}, {"in"}, {"out"}}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::GateNOT>(n, _gi(p, "bit_width", 8));
        });
        f.registerType({"buf", {{"bit_width", "8"}}, {"in"}, {"out"}}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::GateBUF>(n, _gi(p, "bit_width", 8));
        });
        f.registerType({"tsbuf", {{"bit_width", "8"}}, {"in", "oe"}, {"out"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::GateTSBUF>(n, _gi(p, "bit_width", 8));
                       });

        // ============================================================
        // 时序元件
        // ============================================================
        f.registerType({"dff", {{"bit_width", "8"}}, {"d", "clk"}, {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::DFlipFlop>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"tff", {{"bit_width", "8"}}, {"t", "clk"}, {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::TFlipFlop>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"jkff", {{"bit_width", "8"}}, {"j", "k", "clk"}, {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::JKFlipFlop>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"register", {{"bit_width", "8"}}, {"d", "clk"}, {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Register>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"latch", {{"bit_width", "8"}}, {"d", "en"}, {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Latch>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"counter",
                        {{"bit_width", "8"}, {"has_load", "0"}, {"has_en", "0"}, {"has_updown", "0"}, {"has_clr", "0"}},
                        {"clk"},
                        {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Counter>(n, _gi(p, "bit_width", 8), _gi(p, "has_load", 0) != 0,
                                                                 _gi(p, "has_en", 0) != 0, _gi(p, "has_updown", 0) != 0,
                                                                 _gi(p, "has_clr", 0) != 0);
                       });
        f.registerType({"shift", {{"length", "8"}, {"has_dir", "0"}, {"has_clr", "0"}}, {"sin", "clk"}, {"q"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::ShiftRegister>(
                                   n, _gi(p, "length", 8), _gi(p, "has_dir", 0) != 0, _gi(p, "has_clr", 0) != 0);
                       });
        f.registerType({"clkgen", {{"high_ticks", "1"}, {"low_ticks", "1"}}, {}, {"clk"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::ClockGen>(n, _gi(p, "high_ticks", 1), _gi(p, "low_ticks", 1));
                       });
        f.registerType({"print", {{"bit_width", "8"}}, {"in", "clk"}, {}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Print>(n, _gi(p, "bit_width", 8));
                       });

        // ============================================================
        // 运算元件
        // ============================================================
        f.registerType({"mux", {{"n_selects", "1"}, {"bit_width", "8"}}, {}, {"out"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Mux>(n, _gi(p, "n_selects", 1), _gi(p, "bit_width", 8));
                       });
        f.registerType({"adder", {{"bit_width", "8"}}, {"a", "b", "cin"}, {"sum", "cout"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Adder>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"comparator", {{"bit_width", "8"}, {"op", "eq"}, {"signed", "0"}}, {"a", "b"}, {"out"}},
                       [&](const std::string &n, const Params &p) -> C {
                           std::string op = "eq";
                           auto it = p.find("op");
                           if (it != p.end())
                               op = it->second;
                           dsc::CmpOp o = dsc::CmpOp::EQ;
                           if (op == "ne")
                               o = dsc::CmpOp::NE;
                           else if (op == "lt")
                               o = dsc::CmpOp::LT;
                           else if (op == "gt")
                               o = dsc::CmpOp::GT;
                           else if (op == "le")
                               o = dsc::CmpOp::LE;
                           else if (op == "ge")
                               o = dsc::CmpOp::GE;
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), o, _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"decoder", {{"n_selects", "2"}}, {}, {}}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Decoder>(n, _gi(p, "n_selects", 2));
        });
        f.registerType({"encoder", {{"n_selects", "2"}}, {}, {}}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Encoder>(n, _gi(p, "n_selects", 2));
        });
        f.registerType({"sub", {{"bit_width", "8"}}, {"a", "b", "bin"}, {"diff", "bout"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Subtractor>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"mul", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"prod"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Multiplier>(n, _gi(p, "bit_width", 8), _gi(p, "signed", 0) != 0);
                       });

        // ============================================================
        // 杂项元件
        // ============================================================
        f.registerType({"constant", {{"bit_width", "8"}, {"value", "0"}}, {}, {"out"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Constant>(
                                   n, _gi(p, "bit_width", 8),
                                   (uint64_t) std::stoull(p.find("value") != p.end() ? p.find("value")->second : "0",
                                                          nullptr, 0));
                       });
        f.registerType({"switch", {{"bit_width", "8"}}, {"in", "en"}, {"out"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Switch>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"merge", {{"num_bits", "8"}}, {}, {"out"}}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Merge>(n, _gi(p, "num_bits", 8));
        });
        f.registerType({"split", {{"num_bits", "8"}}, {"in"}, {}}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Split>(n, _gi(p, "num_bits", 8));
        });

        // ============================================================
        // 浮点运算
        // ============================================================
        f.registerType({"fadd", {{"precision","32"}}, {"a","b"}, {"out"}},
                       [&](const std::string &n, const Params &p)->C {
                           return std::make_unique<dsc::FloatAdd>(n, _gi(p,"precision",32));
                       });
        f.registerType({"fsub", {{"precision","32"}}, {"a","b"}, {"out"}},
                       [&](const std::string &n, const Params &p)->C {
                           return std::make_unique<dsc::FloatSub>(n, _gi(p,"precision",32));
                       });
        f.registerType({"fmul", {{"precision","32"}}, {"a","b"}, {"out"}},
                       [&](const std::string &n, const Params &p)->C {
                           return std::make_unique<dsc::FloatMul>(n, _gi(p,"precision",32));
                       });
        f.registerType({"fdiv", {{"precision","32"}}, {"a","b"}, {"out"}},
                       [&](const std::string &n, const Params &p)->C {
                           return std::make_unique<dsc::FloatDiv>(n, _gi(p,"precision",32));
                       });
        f.registerType({"fcmp", {{"precision","32"},{"op","eq"}}, {"a","b"}, {"out"}},
                       [&](const std::string &n, const Params &p)->C {
                           std::string op = "eq"; auto it = p.find("op"); if(it!=p.end()) op=it->second;
                           dsc::FloatCmpOp o = dsc::FloatCmpOp::EQ;
                           if(op=="ne") o=dsc::FloatCmpOp::NE; else if(op=="lt") o=dsc::FloatCmpOp::LT;
                           else if(op=="gt") o=dsc::FloatCmpOp::GT; else if(op=="le") o=dsc::FloatCmpOp::LE;
                           else if(op=="ge") o=dsc::FloatCmpOp::GE;
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p,"precision",32), o);
                       });
        f.registerType({"fconst", {{"precision","32"},{"value","0.0"}}, {}, {"out"}},
                       [&](const std::string &n, const Params &p)->C {
                           double v = 0.0;
                           auto it = p.find("value");
                           if(it != p.end()) v = std::stod(it->second);
                           return std::make_unique<dsc::FloatConst>(n, _gi(p,"precision",32), v);
                       });

        // 有符号乘除 / 除法器
        f.registerType({"div", {{"bit_width","8"}}, {"a","b"}, {"quot","rem"}},
                       [&](const std::string &n, const Params &p)->C {
                           return std::make_unique<dsc::Divider>(n, _gi(p,"bit_width",8));
                       });

        // ============================================================
        // 内存 & DLL
        // ============================================================
        f.registerType({"rom",
                        {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "0"}, {"initial_data", ""}},
                        {"addr", "clk"},
                        {"data_out"}},
                       [&](const std::string &n, const Params &p) -> C {
                           std::string init;
                           auto it = p.find("initial_data");
                           if (it != p.end())
                               init = it->second;
                           return std::make_unique<dsc::ROM>(n, _gi(p, "addr_width", 4), _gi(p, "data_width", 8), init,
                                                             _gi(p, "read_latency", 0));
                       });

        f.registerType({"memory",
                        {{"addr_width", "4"}, {"data_width", "8"}, {"read_latency", "0"}, {"write_latency", "0"}},
                        {"addr", "data_in", "we", "clk"},
                        {"data_out", "busy"}},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Memory>(n, _gi(p, "addr_width", 4), _gi(p, "data_width", 8),
                                                                _gi(p, "read_latency", 0), _gi(p, "write_latency", 0));
                       });
        // ============================================================
        // 复合元件
        // ============================================================
        f.registerType({"composite", {{"definition", ""}}, {}, {}}, [&](const std::string &n, const Params &p) -> C {
            std::string def;
            auto it = p.find("definition");
            if (it != p.end())
                def = it->second;
            return std::make_unique<dsc::CompositeComponent>(n, def);
        });

        f.registerType({"dll", {{"path", ""}}, {}, {}}, [&](const std::string &n, const Params &p) -> C {
            std::string path;
            auto it = p.find("path");
            if (it != p.end())
                path = it->second;
            return std::make_unique<dsc::DllComponent>(n, path);
        });
    }
};

static InitAllComponents _init_all;

} // namespace
