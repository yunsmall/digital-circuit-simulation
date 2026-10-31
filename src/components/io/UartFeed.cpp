// UartFeed — 由外部喂数据的 8N1 串行发送器
// 发送状态机展开在 JIT 里，每拍只读一个 volatile 标志决定要不要碰队列；
// 队列本身（SPSC 原子环形缓冲）留在 C++，只在真的有数据时才被调用。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/UartFeed.h"

#include <atomic>
#include <format>

namespace dsc {

UartFeed::UartFeed(const std::string &name, int baud_div) :
    SequentialComponent(name, "uart_feed"), _baud_div(baud_div < 2 ? 2 : baud_div) {
    setParam("baud_div", std::to_string(_baud_div));
    addInput("clk", 1);
    addOutput("rx", 1, false, true);
    _state.head.store(0);
    _state.tail.store(0);
    _state.baud_div = (uint32_t) _baud_div;
}

UartFeed::~UartFeed() = default;

bool UartFeed::prepare(std::string &) { return true; }

std::vector<Component::JitSymbol> UartFeed::extraJitSymbols() const {
    // 只有标志置位时 JIT 才会调用它，正常仿真里这条路径几乎不触发
    return {{"_dcs_feed_pop", reinterpret_cast<void *>(&UartFeed::feedPop)}};
}

std::string UartFeed::genStateDecl() const {
    return std::format(
            "static uint32_t _feed_cnt_{0}, _feed_bit_{0};\n"
            "static uint8_t  _feed_busy_{0}, _feed_ch_{0};\n"
            "static volatile uint32_t *_feed_flag_{0} = (volatile uint32_t *)0x{1:X}ULL;\n",
            _id, reinterpret_cast<uintptr_t>(&_state.has_data));
}

std::string UartFeed::genInitCode() const {
    return std::format("    _feed_cnt_{0} = 0; _feed_bit_{0} = 0; _feed_busy_{0} = 0; _feed_ch_{0} = 0;\n", _id);
}

std::string UartFeed::genFuncDef_seq() const {
    std::string c;
    c += std::format("// UartFeed {}\n", name());
    c += "extern int _dcs_feed_pop(void *);\n";
    c += std::format("DCS_INLINE void {}() {{\n", funcName_seq());
    // 生产者把标志置 1，这里读一次；读不到就整拍跳过，不产生任何调用
    c += std::format(
            "    if (!_feed_busy_{0} && *_feed_flag_{0}) {{\n"
            "        const int _b = _dcs_feed_pop((void *)0x{1:X}ULL);\n"
            "        if (_b >= 0) {{ _feed_ch_{0} = (uint8_t)_b; _feed_busy_{0} = 1; _feed_bit_{0} = 0; _feed_cnt_{0} = 0; }}\n"
            "    }}\n"
            "    uint8_t _level = 1;\n"
            "    if (_feed_busy_{0}) {{\n"
            "        if (_feed_bit_{0} == 0) _level = 0;\n"
            "        else if (_feed_bit_{0} <= 8) _level = (uint8_t)((_feed_ch_{0} >> (_feed_bit_{0} - 1)) & 1u);\n"
            "        if (++_feed_cnt_{0} >= {2}u) {{\n"
            "            _feed_cnt_{0} = 0;\n"
            "            if (++_feed_bit_{0} > 9) _feed_busy_{0} = 0;\n"
            "        }}\n"
            "    }}\n",
            _id, reinterpret_cast<uintptr_t>(&_state), _baud_div);
    c += "    " + genOutputWrite(0, "_level", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> UartFeed::clone(const std::string &new_name) const {
    return std::make_unique<UartFeed>(new_name, _baud_div);
}

bool UartFeed::push(uint8_t b) {
    const uint32_t h = _state.head.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1) % CAP;
    // 队列满：返回 false 让生产者自己决定重试/丢弃，不能悄悄覆盖还没发出去的数据
    if (next == _state.tail.load(std::memory_order_acquire))
        return false;
    _state.buf[h] = b;
    _state.head.store(next, std::memory_order_release);
    // 标志只是一个"可能有数据"的提示，晚一拍被看到不影响正确性
    _state.has_data = 1;
    return true;
}

size_t UartFeed::push(const uint8_t *data, size_t len) {
    size_t n = 0;
    for (; n < len; n++)
        if (!push(data[n]))
            break;
    return n;
}

size_t UartFeed::pending() const {
    const uint32_t h = _state.head.load(std::memory_order_acquire);
    const uint32_t t = _state.tail.load(std::memory_order_acquire);
    return (h + CAP - t) % CAP;
}

int UartFeed::feedPop(void *state_ptr) {
    auto &s = *static_cast<FeedState *>(state_ptr);
    const uint32_t t = s.tail.load(std::memory_order_relaxed);
    if (t == s.head.load(std::memory_order_acquire)) {
        s.has_data = 0; // 队列见底，清掉提示，下一拍 JIT 就不用再进来
        return -1;
    }
    const uint8_t ch = s.buf[t];
    s.tail.store((t + 1) % CAP, std::memory_order_release);
    if ((t + 1) % CAP == s.head.load(std::memory_order_acquire))
        s.has_data = 0;
    return ch;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"uart_feed", {{"baud_div", "16"}}, {"clk"}, {"rx"}, dsc::ComponentCategory::Misc, "串口输入端（外部喂数据）：元件本身不做系统调用，宿主程序 push 字节进来，按 8N1 发给 UART.rx"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UartFeed>(n, dsc::paramInt(p, "baud_div", 16));
        });
} // namespace
