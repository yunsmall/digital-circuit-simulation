// ComponentFactory — 所有元件类型的集中注册
//
// 所有工厂注册集中在此文件，由全局静态对象在 main 前完成。
// ComponentFactory 自身被各处引用，此 .obj 必定被链接，无需 /WHOLEARCHIVE。
#include "dcs/ComponentFactory.h"
#include "dcs/CircuitLibrary.h"
#include "dcs/components/combinational/Arithmetic.h"
#include "dcs/components/combinational/BarrelShifter.h"
#include "dcs/components/platform/Composite.h"
#include "dcs/components/sequential/DelayLine.h"
#include "dcs/components/platform/Dll.h"
#include "dcs/components/memory/FIFO.h"
#include "dcs/components/combinational/FloatPoint.h"
#include "dcs/components/io/FloatPrint.h"
#include "dcs/components/combinational/Gates.h"
#include "dcs/components/memory/Memory.h"
#include "dcs/components/combinational/Misc.h"
#include "dcs/components/memory/MultiPortRAM.h"
#include "dcs/components/io/Print.h"
#include "dcs/components/combinational/PriorityEncoder.h"
#include "dcs/components/combinational/Reduction.h"
#include "dcs/components/memory/ROM.h"
#include "dcs/components/sequential/Sequential.h"
#include "dcs/components/combinational/Shifter.h"
#include "dcs/components/combinational/SignExt.h"
#include "dcs/components/platform/CLINT.h"
#include "dcs/components/platform/AxiLiteBridge.h"
#include "dcs/components/platform/DMA.h"
#include "dcs/components/io/I2C.h"
#include "dcs/components/platform/WishboneCache.h"
#include "dcs/components/platform/PLIC.h"
#include "dcs/components/io/SPI.h"
#include "dcs/components/io/UART.h"
#include "dcs/components/io/RTC.h"
#include "dcs/components/io/Gpio.h"
#include "dcs/components/io/Pwm.h"
#include "dcs/components/io/Watchdog.h"
#include "dcs/components/platform/RegBridge.h"
#include "dcs/components/io/UartTerm.h"
#include "dcs/components/io/UartIn.h"
#include "dcs/components/io/UartFeed.h"
#include "dcs/components/platform/RiscVCPU.h"
#include "dcs/components/memory/WishboneMemory.h"
#include "dcs/components/memory/WishboneROM.h"
#include "dcs/components/platform/WishboneInterconnect.h"

#include <charconv>
#include <filesystem>
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
    if (it == p.end())
        return d;
    // 用 from_chars 而不是 stoi：JSON 里把整型参数写成非数字时，stoi 直接抛
    // std::invalid_argument，一路穿到 main 把进程打掉。这里退回默认值，
    // 真正的非法值由 validateParams 和元件构造后的参数校验收口
    const auto &s = it->second;
    int v = d;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return (ec == std::errc{} && ptr == s.data() + s.size()) ? v : d;
}

// 字符串参数解析
static std::string _gs(const Params &p, const std::string &k, const std::string &d) {
    auto it = p.find(k);
    return it != p.end() ? it->second : d;
}

struct InitAllComponents {
    InitAllComponents() {
        auto &f = dsc::ComponentFactory::instance();
        using C = std::unique_ptr<dsc::Component>;

        // ============================================================
        // 门电路
        // ============================================================
        f.registerType({"and", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入与门，所有输入为1时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::LogicGate>(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8),
                                                                    dsc::GateOp::AND);
                       });
        f.registerType({"or", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入或门，任一输入为1时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::LogicGate>(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8),
                                                                    dsc::GateOp::OR);
                       });
        f.registerType({"nand", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入与非门，相当于与门+非门"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::LogicGate>(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8),
                                                                    dsc::GateOp::NAND);
                       });
        f.registerType({"nor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入或非门，相当于或门+非门"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::LogicGate>(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8),
                                                                    dsc::GateOp::NOR);
                       });
        f.registerType({"xor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入异或门，输入中1的个数为奇数时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::LogicGate>(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8),
                                                                    dsc::GateOp::XOR);
                       });
        f.registerType({"xnor", {{"inputs", "2"}, {"bit_width", "8"}}, {"in0", "in1"}, {"out"}, dsc::ComponentCategory::Gate, "多输入同或门，相当于异或门+非门"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::LogicGate>(n, _gi(p, "inputs", 2), _gi(p, "bit_width", 8),
                                                                    dsc::GateOp::XNOR);
                       });
        f.registerType({"not", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Gate, "非门/反相器，输出=~输入"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::UnaryGate>(n, _gi(p, "bit_width", 8), true);
        });
        f.registerType({"buf", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Gate, "缓冲器，输出=输入"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::UnaryGate>(n, _gi(p, "bit_width", 8), false);
        });
        f.registerType({"tsbuf", {{"bit_width", "8"}}, {"in", "oe"}, {"out"}, dsc::ComponentCategory::Gate, "三态缓冲器，oe=1时输出=输入，oe=0时高阻"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::GateTSBUF>(n, _gi(p, "bit_width", 8));
                       });

        // ============================================================
        // 绝对值 / 最小最大值
        // ============================================================
        f.registerType({"abs", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Arithmetic, "绝对值，输出=|输入|"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Abs>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"min",
                        {{"bit_width", "8"}, {"signed", "0"}},
                        {"a", "b"},
                        {"out"},
                        dsc::ComponentCategory::Arithmetic, "最小值，输出=min(a,b)"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::MinMax>(n, _gi(p, "bit_width", 8), false,
                                                                _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"max",
                        {{"bit_width", "8"}, {"signed", "0"}},
                        {"a", "b"},
                        {"out"},
                        dsc::ComponentCategory::Arithmetic, "最大值，输出=max(a,b)"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::MinMax>(n, _gi(p, "bit_width", 8), true,
                                                                _gi(p, "signed", 0) != 0);
                       });

        // ============================================================
        // 归约门
        // ============================================================
        f.registerType({"redand", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Reduction, "归约与，输出=所有输入位的AND"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Reduction>(n, _gi(p, "bit_width", 8), dsc::ReductionOp::AND);
                       });
        f.registerType({"redor", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Reduction, "归约或，输出=所有输入位的OR"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Reduction>(n, _gi(p, "bit_width", 8), dsc::ReductionOp::OR);
                       });
        f.registerType({"redxor", {{"bit_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::Reduction, "归约异或，输出=所有输入位的XOR"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Reduction>(n, _gi(p, "bit_width", 8), dsc::ReductionOp::XOR);
                       });

        // ============================================================
        // 时序元件
        // ============================================================
        f.registerType({"dff", {{"bit_width", "8"}}, {"d", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "D触发器，clk上升沿采样d输出到q"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::DFlipFlop>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"tff", {{"bit_width", "8"}}, {"t", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "T触发器，clk上升沿且t=1时翻转"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::TFlipFlop>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"jkff", {{"bit_width", "8"}}, {"j", "k", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "JK触发器，j/k控制置位/复位，j=k=1时翻转"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::JKFlipFlop>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"register", {{"bit_width", "8"}}, {"d", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "寄存器，clk上升沿采样d"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Register>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"latch", {{"bit_width", "8"}}, {"d", "en"}, {"q"}, dsc::ComponentCategory::Sequential, "锁存器，en=1时透明，en=0时锁存"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Latch>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"counter",
                        {{"bit_width", "8"}, {"has_load", "0"}, {"has_en", "0"}, {"has_updown", "0"}, {"has_clr", "0"}},
                        {"clk"},
                        {"q"},
                        dsc::ComponentCategory::Sequential, "计数器，支持加载/使能/升降方向/清零"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Counter>(n, _gi(p, "bit_width", 8), _gi(p, "has_load", 0) != 0,
                                                                 _gi(p, "has_en", 0) != 0, _gi(p, "has_updown", 0) != 0,
                                                                 _gi(p, "has_clr", 0) != 0);
                       });
        f.registerType({"shift", {{"length", "8"}, {"has_dir", "0"}, {"has_clr", "0"}}, {"sin", "clk"}, {"q"}, dsc::ComponentCategory::Sequential, "移位寄存器，支持串入/方向切换/清零"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::ShiftRegister>(
                                   n, _gi(p, "length", 8), _gi(p, "has_dir", 0) != 0, _gi(p, "has_clr", 0) != 0);
                       });
        f.registerType({"clkgen", {{"high_ticks", "1"}, {"low_ticks", "1"}}, {}, {"clk"}, dsc::ComponentCategory::Sequential, "时钟发生器，可设高/低电平持续tick数"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::ClockGen>(n, _gi(p, "high_ticks", 1), _gi(p, "low_ticks", 1));
                       });
        f.registerType({"edgedet_rise", {{}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential, "上升沿检测，in上升沿时out=1一个周期"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::EdgeDetect>(n, dsc::EdgeType::Rising);
                       });
        f.registerType({"edgedet_fall", {{}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential, "下降沿检测，in下降沿时out=1一个周期"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::EdgeDetect>(n, dsc::EdgeType::Falling);
                       });
        f.registerType({"edgedet_both", {{}}, {"clk", "in"}, {"out"}, dsc::ComponentCategory::Sequential, "双边沿检测，in任意边沿时out=1一个周期"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::EdgeDetect>(n, dsc::EdgeType::Both);
                       });
        f.registerType({"clkdiv", {{"divisor", "2"}}, {"clk"}, {"out"}, dsc::ComponentCategory::Sequential, "时钟分频器，每N个clk周期翻转一次"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::ClockDivider>(n, _gi(p, "divisor", 2));
                       });
        f.registerType({"print", {{"bit_width", "8"}, {"ascii", "0", {"0", "1"}}}, {"in", "clk", "en"}, {}, dsc::ComponentCategory::Misc, "整型打印，clk上升沿打印当前值（en 为高才打印，ascii=1 时按字符输出）"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Print>(n, _gi(p, "bit_width", 8), _gs(p, "ascii", "0") == "1");
                       });
        f.registerType({"fprint", {{"precision", "32", {"32", "64"}}}, {"in", "clk"}, {}, dsc::ComponentCategory::Float, "浮点打印，clk上升沿打印浮点值"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatPrint>(n, _gi(p, "precision", 32));
                       });
        f.registerType({"delay", {{"bit_width", "8"}, {"depth", "1"}, {"use_clock", "1"}}, {"in"}, {"out"}, dsc::ComponentCategory::DataPath, "延迟线，可配深度和时钟使能的流水线延迟"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::DelayLine>(n, _gi(p, "bit_width", 8), _gi(p, "depth", 1),
                                                                   _gi(p, "use_clock", 1) != 0);
                       });

        // ============================================================
        // 运算元件
        // ============================================================
        f.registerType({"mux", {{"n_selects", "1"}, {"bit_width", "8"}}, {}, {"out"}, dsc::ComponentCategory::DataPath, "多路选择器，根据sel选择一路输入输出"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Mux>(n, _gi(p, "n_selects", 1), _gi(p, "bit_width", 8));
                       });
        f.registerType({"adder", {{"bit_width", "8"}}, {"a", "b", "cin"}, {"sum", "cout"}, dsc::ComponentCategory::Arithmetic, "全加器，sum=a+b+cin, cout=进位"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Adder>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"eq", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "等于比较，a==b时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), dsc::CmpOp::EQ,
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"ne", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "不等于比较，a!=b时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), dsc::CmpOp::NE,
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"lt", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "小于比较，a<b时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), dsc::CmpOp::LT,
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"gt", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "大于比较，a>b时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), dsc::CmpOp::GT,
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"le", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "小于等于比较，a<=b时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), dsc::CmpOp::LE,
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"ge", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Arithmetic, "大于等于比较，a>=b时输出1"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Comparator>(n, _gi(p, "bit_width", 8), dsc::CmpOp::GE,
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"decoder", {{"n_selects", "2"}}, {}, {}, dsc::ComponentCategory::DataPath, "译码器，n位地址→2^n位独热码"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Decoder>(n, _gi(p, "n_selects", 2));
        });
        f.registerType({"encoder", {{"n_selects", "2"}}, {}, {}, dsc::ComponentCategory::DataPath, "编码器，2^n位独热码→n位二进制"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Encoder>(n, _gi(p, "n_selects", 2));
        });
        f.registerType({"sub", {{"bit_width", "8"}}, {"a", "b", "bin"}, {"diff", "bout"}, dsc::ComponentCategory::Arithmetic, "全减器，diff=a-b-bin, bout=借位"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Subtractor>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"mul", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"prod_lo", "prod_hi"}, dsc::ComponentCategory::Arithmetic, "乘法器，prod=a*b（双倍位宽输出）"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Multiplier>(n, _gi(p, "bit_width", 8),
                                                                    _gi(p, "signed", 0) != 0);
                       });

        // ============================================================
        // 数据通路元件
        // ============================================================
        f.registerType({"signext", {{"src_width", "4"}, {"dst_width", "8"}}, {"in"}, {"out"}, dsc::ComponentCategory::DataPath, "符号扩展，将窄位宽有符号数扩展到宽位宽"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::SignExt>(n, _gi(p, "src_width", 4), _gi(p, "dst_width", 8));
                       });
        f.registerType({"barrel", {{"bit_width", "8"}}, {"in", "amt", "dir", "arith"}, {"out"}, dsc::ComponentCategory::DataPath, "桶形移位器，支持左移/逻辑右移/算术右移"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::BarrelShifter>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"lsl",
                        {{"bit_width", "8"}},
                        {"in", "shift"},
                        {"out"},
                        dsc::ComponentCategory::DataPath, "逻辑左移，out=in<<shift，低位补0"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Shifter>(n, _gi(p, "bit_width", 8), dsc::ShiftMode::LSL);
                       });
        f.registerType({"lsr",
                        {{"bit_width", "8"}},
                        {"in", "shift"},
                        {"out"},
                        dsc::ComponentCategory::DataPath, "逻辑右移，out=in>>shift，高位补0"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Shifter>(n, _gi(p, "bit_width", 8), dsc::ShiftMode::LSR);
                       });
        f.registerType({"asr",
                        {{"bit_width", "8"}},
                        {"in", "shift"},
                        {"out"},
                        dsc::ComponentCategory::DataPath, "算术右移，out=in>>shift，高位补符号位"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Shifter>(n, _gi(p, "bit_width", 8), dsc::ShiftMode::ASR);
                       });
        f.registerType({"prienc", {{"num_inputs", "8"}}, {}, {"out", "valid"}, dsc::ComponentCategory::DataPath, "优先编码器，输出最高位1的位置"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::PriorityEncoder>(n, _gi(p, "num_inputs", 8));
                       });

        // ============================================================
        // 杂项元件
        // ============================================================
        f.registerType({"constant", {{"bit_width", "8"}, {"value", "0"}}, {}, {"out"}, dsc::ComponentCategory::DataPath, "常量，输出固定值"},
                       [&](const std::string &n, const Params &p) -> C {
                           uint64_t v = 0;
                           auto it = p.find("value");
                           if (it != p.end()) {
                               const auto &s = it->second;
                               auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 0);
                               // 解析失败就当 0：非数字由 validateParams 报错，不该在这抛
                               if (ec != std::errc{} || ptr != s.data() + s.size())
                                   v = 0;
                           }
                           return std::make_unique<dsc::Constant>(n, _gi(p, "bit_width", 8), v);
                       });
        f.registerType({"switch", {{"bit_width", "8"}}, {"in", "en"}, {"out"}, dsc::ComponentCategory::DataPath, "开关，en=1时out=in，en=0时高阻"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Switch>(n, _gi(p, "bit_width", 8));
                       });
        f.registerType({"merge", {{"num_bits", "8"}}, {}, {"out"}, dsc::ComponentCategory::DataPath, "位合并，将num_bits个1位输入合并为多bit输出"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Merge>(n, _gi(p, "num_bits", 8));
        });
        f.registerType({"split", {{"num_bits", "8"}}, {"in"}, {}, dsc::ComponentCategory::DataPath, "位拆分，将多bit输入拆分为num_bits个1位输出"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::Split>(n, _gi(p, "num_bits", 8));
        });

        // ============================================================
        // 浮点运算
        // ============================================================
        f.registerType({"fadd", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点加法，out=a+b"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatBinOp>(n, _gi(p, "precision", 32), dsc::FloatBinOpKind::ADD);
                       });
        f.registerType({"fsub", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点减法，out=a-b"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatBinOp>(n, _gi(p, "precision", 32), dsc::FloatBinOpKind::SUB);
                       });
        f.registerType({"fmul", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点乘法，out=a*b"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatBinOp>(n, _gi(p, "precision", 32), dsc::FloatBinOpKind::MUL);
                       });
        f.registerType({"fdiv", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点除法，out=a/b"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatBinOp>(n, _gi(p, "precision", 32), dsc::FloatBinOpKind::DIV);
                       });
        f.registerType({"feq", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点等于比较"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p, "precision", 32), dsc::FloatCmpOp::EQ);
                       });
        f.registerType({"fne", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点不等于比较"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p, "precision", 32), dsc::FloatCmpOp::NE);
                       });
        f.registerType({"flt", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点小于比较"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p, "precision", 32), dsc::FloatCmpOp::LT);
                       });
        f.registerType({"fgt", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点大于比较"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p, "precision", 32), dsc::FloatCmpOp::GT);
                       });
        f.registerType({"fle", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点小于等于比较"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p, "precision", 32), dsc::FloatCmpOp::LE);
                       });
        f.registerType({"fge", {{"precision", "32", {"32", "64"}}}, {"a", "b"}, {"out"}, dsc::ComponentCategory::Float, "浮点大于等于比较"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatCmp>(n, _gi(p, "precision", 32), dsc::FloatCmpOp::GE);
                       });
        f.registerType({"fconst", {{"precision", "32", {"32", "64"}}, {"value", "0.0"}}, {}, {"out"}, dsc::ComponentCategory::Float, "浮点常量"},
                       [&](const std::string &n, const Params &p) -> C {
                           double v = 0.0;
                           auto it = p.find("value");
                           if (it != p.end())
                               v = std::stod(it->second);
                           return std::make_unique<dsc::FloatConst>(n, _gi(p, "precision", 32), v);
                       });

        // ============================================================
        // 浮点 ↔ 整型转换
        // ============================================================
        f.registerType({"f2i",
                        {{"fp", "32", {"32", "64"}}, {"int_width", "8"}, {"signed", "0"}},
                        {"in"},
                        {"out"},
                        dsc::ComponentCategory::Float, "浮点转整型，截断小数部分"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FloatToInt>(n, _gi(p, "fp", 32), _gi(p, "int_width", 8),
                                                                    _gi(p, "signed", 0) != 0);
                       });
        f.registerType({"i2f",
                        {{"int_width", "8"}, {"fp", "32", {"32", "64"}}, {"signed", "0"}},
                        {"in"},
                        {"out"},
                        dsc::ComponentCategory::Float, "整型转浮点"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::IntToFloat>(n, _gi(p, "int_width", 8), _gi(p, "fp", 32),
                                                                    _gi(p, "signed", 0) != 0);
                       });

        // 有符号乘除 / 除法器
        f.registerType({"div", {{"bit_width", "8"}, {"signed", "0"}}, {"a", "b"}, {"quot", "rem"}, dsc::ComponentCategory::Arithmetic, "除法器，quot=a/b, rem=a%b"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Divider>(n, _gi(p, "bit_width", 8), _gi(p, "signed", 0) != 0);
                       });

        // ============================================================
        // 内存 & DLL
        // ============================================================
        f.registerType({"rom",
                        {{"addr_width", "4"},
                         {"data_width", "8", {"8", "16", "32", "64"}},
                         {"read_latency", "0"},
                         {"source_type", "mem", {"mem", "file"}},
                         {"filepath", ""}},
                        {"addr", "clk"},
                        {"data_out"},
                        dsc::ComponentCategory::Memory, "只读存储器，内存模式通过IStorage写入，文件模式从二进制文件加载"},
                       [&](const std::string &n, const Params &p) -> C {
                           int aw = _gi(p, "addr_width", 4);
                           int dw = _gi(p, "data_width", 8);
                           int rl = _gi(p, "read_latency", 0);
                           std::string st = _gs(p, "source_type", "mem");

                           if (st == "file") {
                               return std::make_unique<dsc::ROM>(n, aw, dw, std::filesystem::path(_gs(p, "filepath", "")), rl);
                           }
                           return std::make_unique<dsc::ROM>(n, aw, dw, rl);
                       });

        f.registerType({"memory",
                        {{"addr_width", "4"}, {"data_width", "8", {"8", "16", "32", "64"}}, {"read_latency", "0"}, {"write_latency", "0"}},
                        {"addr", "data_in", "we", "clk"},
                        {"data_out", "busy"},
                        dsc::ComponentCategory::Memory, "可寻址RAM，支持独立配置读写延迟"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::Memory>(n, _gi(p, "addr_width", 4), _gi(p, "data_width", 8),
                                                                _gi(p, "read_latency", 0), _gi(p, "write_latency", 0));
                       });
        f.registerType({"mpram",
                        {{"addr_width", "4"},
                         {"data_width", "8", {"8", "16", "32", "64"}},
                         {"num_read_ports", "1"},
                         {"num_write_ports", "1"},
                         {"read_latency", "0"}},
                        {},
                        {},
                        dsc::ComponentCategory::Memory, "多端口RAM，支持多个独立读写端口和流水线延迟"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::MultiPortRAM>(
                                   n, _gi(p, "addr_width", 4), _gi(p, "data_width", 8), _gi(p, "num_read_ports", 1),
                                   _gi(p, "num_write_ports", 1), _gi(p, "read_latency", 0));
                       });
        f.registerType({"fifo",
                        {{"data_width", "8", {"8", "16", "32", "64"}}, {"depth", "16"}, {"has_rst", "0"}},
                        {"din", "wr_en", "rd_en", "clk"},
                        {"dout", "full", "empty"},
                        dsc::ComponentCategory::Memory, "同步FIFO，先入先出队列，支持异步复位"},
                       [&](const std::string &n, const Params &p) -> C {
                           return std::make_unique<dsc::FIFO>(n, _gi(p, "data_width", 8), _gi(p, "depth", 16),
                                                              _gi(p, "has_rst", 0) != 0);
                       });

        // ============================================================
        // 复合元件
        // ============================================================
        f.registerType({"composite", {{"definition", ""}}, {}, {}, dsc::ComponentCategory::Misc, "复合元件，由JSON定义嵌套子电路"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::CompositeComponent>(n, _gs(p, "definition", ""));
        });

        f.registerType({"dll", {{"path", ""}}, {}, {}, dsc::ComponentCategory::Misc, "外部DLL元件，由动态库提供仿真逻辑"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::DllComponent>(n, _gs(p, "path", ""));
        });

        // ============================================================
        // RISC-V CPU + Wishbone 总线
        // ============================================================
        f.registerType({"riscv_cpu", {{"xlen", "32", {"32", "64"}}},
                        {"clk", "rst", "wb_dat_i", "wb_ack_i", "irq", "mti", "msi", "seip"},
                        {"wb_adr_o", "wb_dat_o", "wb_we_o", "wb_sel_o", "wb_stb_o", "wb_cyc_o", "halted"},
                        dsc::ComponentCategory::CPU,
                        "RISC-V 软核处理器，Wishbone 总线主设备，支持外部/定时器/软件中断；"
                        "xlen 选 32（RV32IMA）或 64（RV64IMA）"}, [&](const std::string &n, const Params &p) -> C {
            if (_gs(p, "xlen", "32") == "64")
                return std::make_unique<dsc::RiscVCPU<64>>(n);
            return std::make_unique<dsc::RiscVCPU<32>>(n);
        });

        f.registerType({"wishbone_mem", {{"addr_width", "16"}, {"data_width", "32", {"8", "16", "32", "64"}}}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "clk"}, {"wb_dat_o", "wb_ack_o"}, dsc::ComponentCategory::Memory, "Wishbone 从设备存储器，支持 IStorage 加载程序"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::WishboneMemory>(n, _gi(p, "addr_width", 16), _gi(p, "data_width", 32));
        });

        f.registerType({"wishbone_rom_adapter", {{"addr_width", "16"}, {"data_width", "32", {"8", "16", "32", "64"}}, {"read_latency", "0"}}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "clk", "rom_data"}, {"wb_dat_o", "wb_ack_o", "rom_addr"}, dsc::ComponentCategory::Bus, "ROM 的 Wishbone 适配器，插在 ROM 与 Wishbone 总线之间。适配器 rom_addr→ROM.addr, ROM.data_out→适配器 rom_data，适配器须在 ROM 之后加入电路"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::WishboneROMAdapter>(n, _gi(p, "addr_width", 16), _gi(p, "data_width", 32),
                                                             _gi(p, "read_latency", 0));
        });

        f.registerType({"wishbone_ic", {{"segments", "0:65536"}, {"num_masters", "1"}}, {"m0_adr_i", "m0_dat_i", "m0_we_i", "m0_sel_i", "m0_stb_i", "m0_cyc_i", "s0_dat_i", "s0_ack_i"}, {"m0_dat_o", "m0_ack_o", "s0_adr_o", "s0_dat_o", "s0_we_o", "s0_sel_o", "s0_stb_o", "s0_cyc_o"}, dsc::ComponentCategory::Bus, "Wishbone 地址解码器/MMIO路由器，按地址段分发到多个从设备"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::WishboneInterconnect>(n, _gs(p, "segments", "0:65536"), _gi(p, "num_masters", 1));
        });

        f.registerType({"clint", {}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "clk"}, {"wb_dat_o", "wb_ack_o", "mti", "msi"}, dsc::ComponentCategory::CPU, "RISC-V CLINT 定时器/软件中断发生器"}, [&](const std::string &n, const Params & /*p*/) -> C {
            return std::make_unique<dsc::CLINT>(n);
        });

        f.registerType({"axi_lite_bridge", {}, {"s_axi_awaddr", "s_axi_awvalid", "s_axi_wdata", "s_axi_wstrb", "s_axi_wvalid", "s_axi_bready", "s_axi_araddr", "s_axi_arvalid", "s_axi_rready", "wb_dat_i", "wb_ack_i"}, {"s_axi_awready", "s_axi_wready", "s_axi_bresp", "s_axi_bvalid", "s_axi_arready", "s_axi_rdata", "s_axi_rresp", "s_axi_rvalid", "wb_adr_o", "wb_dat_o", "wb_we_o", "wb_sel_o", "wb_stb_o", "wb_cyc_o"}, dsc::ComponentCategory::Bus, "AXI4-Lite 从设备到 Wishbone 主设备的桥"}, [&](const std::string &n, const Params &) -> C {
            return std::make_unique<dsc::AxiLiteBridge>(n);
        });

        f.registerType({"dma", {}, {"cfg_adr_i", "cfg_dat_i", "cfg_we_i", "cfg_sel_i", "cfg_stb_i", "cfg_cyc_i", "m_dat_i", "m_ack_i"}, {"cfg_dat_o", "cfg_ack_o", "m_adr_o", "m_dat_o", "m_we_o", "m_sel_o", "m_stb_o", "m_cyc_o"}, dsc::ComponentCategory::Bus, "存储器到存储器块搬运的 DMA 控制器"}, [&](const std::string &n, const Params &) -> C {
            return std::make_unique<dsc::DMA>(n);
        });

        f.registerType({"wb_cache", {{"lines", "16"}}, {"wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "wb_dat_o_i", "wb_ack_o_i"}, {"wb_dat_o", "wb_ack_o", "wb_adr_o", "wb_dat_i_o", "wb_we_o", "wb_sel_o", "wb_stb_o", "wb_cyc_o"}, dsc::ComponentCategory::Memory, "直接映射数据缓存，串在 CPU 与存储器之间"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::WishboneCache>(n, _gi(p, "lines", 16));
        });

        f.registerType({"i2c", {{"clkdiv", "8"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "sda_in"}, {"wb_dat_o", "wb_ack_o", "scl", "sda_out", "sda_oe"}, dsc::ComponentCategory::Misc, "I2C 主机，一次起始-地址-数据-停止的写事务"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::I2C>(n, _gi(p, "clkdiv", 8));
        });

        f.registerType({"spi", {{"clkdiv", "4"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "miso"}, {"wb_dat_o", "wb_ack_o", "sclk", "mosi", "cs"}, dsc::ComponentCategory::Misc, "SPI 主机（模式 0），按 clkdiv 分频收发 8 位"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::SPI>(n, _gi(p, "clkdiv", 4));
        });

        f.registerType({"uart", {{"baud_div", "8"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "rx"}, {"wb_dat_o", "wb_ack_o", "tx", "irq"}, dsc::ComponentCategory::Misc, "16550A 兼容 8N1 串口（reg-shift=2），按 baud_div 个 tick 一位"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::UART>(n, _gi(p, "baud_div", 8));
        });

        f.registerType({"uart_term", {{"baud_div", "16"}}, {"clk", "rx"}, {"data", "valid"}, dsc::ComponentCategory::Misc, "串口终端接收端：把 8N1 电平解成字节，data/valid 接给 print（ascii=1）"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::UartTerm>(n, _gi(p, "baud_div", 16));
        });

        // 下面这几个是总线无关的寄存器元件：addr/wdata/we/en → rdata/ready。
        // 接 Wishbone 用 reg_bridge 转换，接 AXI-Lite 时另加对应适配器
        f.registerType({"rtc", {{"ns_per_tick", "100"}}, {"clk", "addr", "wdata", "we", "en"}, {"rdata", "ready", "irq"}, dsc::ComponentCategory::Misc, "goldfish 风格实时时钟：时间寄存器单位纳秒，带闹钟中断"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::RTC>(n, (uint32_t) _gi(p, "ns_per_tick", 100));
        });

        f.registerType({"gpio", {}, {"clk", "addr", "wdata", "we", "en", "gpio_in"}, {"rdata", "ready", "gpio_out"}, dsc::ComponentCategory::Misc, "32 位 GPIO：DIR/OUT/IN 三个寄存器，gpio_in/gpio_out 为外部引脚"}, [&](const std::string &n, const Params &) -> C {
            return std::make_unique<dsc::Gpio>(n);
        });

        f.registerType({"pwm", {}, {"clk", "addr", "wdata", "we", "en"}, {"rdata", "ready", "pwm"}, dsc::ComponentCategory::Misc, "单通道 PWM：PERIOD/DUTY/CTRL 寄存器，输出方波引脚"}, [&](const std::string &n, const Params &) -> C {
            return std::make_unique<dsc::Pwm>(n);
        });

        f.registerType({"watchdog", {}, {"clk", "addr", "wdata", "we", "en"}, {"rdata", "ready", "irq"}, dsc::ComponentCategory::Misc, "看门狗定时器：CTRL/TIMEOUT/FEED/STATUS，超时拉 irq"}, [&](const std::string &n, const Params &) -> C {
            return std::make_unique<dsc::Watchdog>(n);
        });

        f.registerType({"reg_bridge", {}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "rdata", "ready"}, {"wb_dat_o", "wb_ack_o", "addr", "wdata", "we", "en"}, dsc::ComponentCategory::Misc, "Wishbone 从设备转通用寄存器接口（addr/wdata/we/en → rdata/ready）"}, [&](const std::string &n, const Params &) -> C {
            return std::make_unique<dsc::RegBridge>(n);
        });

        f.registerType({"uart_in", {{"baud_div", "16"}}, {"clk"}, {"rx"}, dsc::ComponentCategory::Misc, "串口输入端：把宿主 stdin 的字符按 8N1 发到 UART 的 rx 引脚（baud_div 要和 UART 一致）"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::UartIn>(n, _gi(p, "baud_div", 16));
        });

        f.registerType({"uart_feed", {{"baud_div", "16"}}, {"clk"}, {"rx"}, dsc::ComponentCategory::Misc, "串口输入端（外部喂数据）：元件本身不做系统调用，宿主程序 push 字节进来，按 8N1 发给 UART.rx"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::UartFeed>(n, _gi(p, "baud_div", 16));
        });

        f.registerType({"plic", {{"num_sources", "8"}}, {"clk", "wb_adr_i", "wb_dat_i", "wb_we_i", "wb_sel_i", "wb_stb_i", "wb_cyc_i", "irq_src"}, {"wb_dat_o", "wb_ack_o", "irq"}, dsc::ComponentCategory::CPU, "RISC-V PLIC 平台级中断控制器，单 context"}, [&](const std::string &n, const Params &p) -> C {
            return std::make_unique<dsc::PLIC>(n, _gi(p, "num_sources", 8));
        });
    }
};

static InitAllComponents _init_all;

} // namespace
