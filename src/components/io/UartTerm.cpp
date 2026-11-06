// UartTerm — 串口终端接收端：只做解串，不负责输出
// 解码状态机全部展开在 JIT 里；统计计数放在 C++ 侧，JIT 写的是它们的内存地址。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/UartTerm.h"

#include <cmath>
#include <format>

namespace dsc {

namespace {
enum InIdx { I_CLK = 0, I_RX };
enum OutIdx { O_DATA = 0, O_VALID };
} // namespace

UartTerm::UartTerm(const std::string &name, int ns_per_period, std::uint64_t baud) :
    SequentialComponent(name, "uart_term"), _ns_per_period(ns_per_period < 1 ? 1 : ns_per_period),
    _baud(baud < 1 ? 1 : baud) {
    _cycles_per_bit = (int) std::llround(1e9 / ((double) _baud * _ns_per_period));
    if (_cycles_per_bit < 2)
        _cycles_per_bit = 2;
    setParam("ns_per_period", std::to_string(_ns_per_period));
    setParam("baud", std::to_string(_baud));
    addInput("clk", 1, PinEdge::Rising);
    addInput("rx", 1);
    addOutput("data", 8, false, true);
    addOutput("valid", 1, false, true);
}

UartTerm::~UartTerm() = default;


std::string UartTerm::genStateDecl() const {
    return std::format(
            "static uint8_t  _term_active_{0}, _term_confirm_{0}, _term_bit_{0}, _term_prev_rx_{0};\n"
            "static uint32_t _term_cnt_{0}, _term_shift_{0};\n"
            "static uint64_t *_term_frames_{0} = (uint64_t *)0x{1:X}ULL;\n"
            "static uint64_t *_term_errors_{0} = (uint64_t *)0x{2:X}ULL;\n"
            "static uint8_t  *_term_last_{0} = (uint8_t *)0x{3:X}ULL;\n",
            _id, reinterpret_cast<uintptr_t>(&_frames), reinterpret_cast<uintptr_t>(&_errors),
            reinterpret_cast<uintptr_t>(&_last_byte));
}

std::string UartTerm::genInitCode() const {
    return std::format(
            "    _term_active_{0} = 0; _term_confirm_{0} = 0; _term_bit_{0} = 0;\n"
            "    _term_cnt_{0} = 0; _term_shift_{0} = 0;\n"
            // 串口线空闲时是高电平；初始成 false 会让第一拍把 0->1 当成上升沿处理
            "    _term_prev_rx_{0} = 1;\n"
            "    *_term_frames_{0} = 0; *_term_errors_{0} = 0; *_term_last_{0} = 0;\n",
            _id);
}

std::string UartTerm::genFuncDef_seq() const {
    const int half = _cycles_per_bit / 2;
    std::string c;
    c += std::format("// UartTerm {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    const bool _clk_pos = " + genGetWireEdge(I_CLK, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";
    // 悬空输入读到 0，不能直接下标 -1。但串口空闲是高电平，0 会被当成起始位，
    // 所以 rx 未连接时按空闲高处理（真实线路靠上拉）
    const auto *rx_pin = inputs()[I_RX].get();
    const int rx_nw = rx_pin->net() ? rx_pin->net()->bit_width() : 0;
    c += "    " + (rx_pin->net() ? gen_read_wire(rx_pin->netId(), 1, rx_nw, "_rx") : std::string("bool _rx = true;")) + "\n";
    c += std::format("    uint8_t _bv = 0; uint8_t _lb = *_term_last_{0};\n", _id);
    // 起始沿 -> 半位确认（挡掉窄脉冲噪声）-> 每位中点采样 -> 停止位校验。
    // 位宽以 tick 计，必须和对面 UART 的 baud_div 一致
    c += std::format(
            "    if (!_term_active_{0}) {{\n"
            "        if (_term_prev_rx_{0} && !_rx) {{\n"
            "            _term_active_{0} = 1; _term_confirm_{0} = 1;\n"
            "            _term_cnt_{0} = 0; _term_bit_{0} = 0; _term_shift_{0} = 0;\n"
            "        }}\n"
            "    }} else if (++_term_cnt_{0} >= (_term_confirm_{0} ? {1}u : {2}u)) {{\n"
            "        _term_cnt_{0} = 0;\n"
            "        if (_term_confirm_{0}) {{\n"
            "            _term_confirm_{0} = 0;\n"
            "            if (_rx) _term_active_{0} = 0;\n" // 半位处已经回高，是噪声不是起始位
            "        }} else if (_term_bit_{0} < 8) {{\n"
            "            _term_shift_{0} |= (uint32_t)(_rx ? 1u : 0u) << _term_bit_{0};\n"
            "            _term_bit_{0}++;\n"
            "        }} else {{\n"
            "            _term_active_{0} = 0;\n"
            "            if (_rx) {{\n",
            _id, half, _cycles_per_bit);
    // 停止位为高才算一帧完整数据
    c += std::format("                _lb = (uint8_t)(_term_shift_{0} & 0xFFu);\n"
                     "                (*_term_frames_{0})++;\n"
                     "                _bv = 1;\n"
                     "            }} else (*_term_errors_{0})++;\n"
                     "        }}\n"
                     "    }}\n"
                     "    _term_prev_rx_{0} = _rx;\n"
                     "    *_term_last_{0} = _lb;\n",
                     _id);
    c += "    " + genOutputWrite(O_DATA, "_lb", 8) + "\n";
    c += "    " + genOutputWrite(O_VALID, "_bv", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> UartTerm::clone(const std::string &new_name) const {
    auto t = std::make_unique<UartTerm>(new_name, _ns_per_period, _baud);
    t->_frames = _frames;
    t->_errors = _errors;
    t->_last_byte = _last_byte;
    return t;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"uart_term", {{"ns_per_period", "20"}, {"baud", "115200"}}, {"clk", "rx"}, {"data", "valid"}, dsc::ComponentCategory::Misc, "串口终端接收端：按 ns_per_period/baud 把 8N1 电平解成字节，data/valid 接给 print（ascii=1）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UartTerm>(n, dsc::paramInt(p, "ns_per_period", 20),
                                                   (std::uint64_t) dsc::paramInt(p, "baud", 115200));
        });
} // namespace
