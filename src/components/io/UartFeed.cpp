// UartFeed — 由外部喂数据的 8N1 串行发送器
// 发送状态机展开在 JIT 里，每拍只比较两个原子 head/tail 决定要不要碰队列；
// 队列本身（SPSC 原子环形缓冲）留在 C++，只在真的有数据时才被调用。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/UartFeed.h"

#include <atomic>
#include <cmath>
#include <format>

namespace dsc {

UartFeed::UartFeed(const std::string &name, int ns_per_period, std::uint64_t baud) :
    SequentialComponent(name, "uart_feed"), _ns_per_period(ns_per_period < 1 ? 1 : ns_per_period),
    _baud(baud < 1 ? 1 : baud) {
    _cycles_per_bit = (int) std::llround(1e9 / ((double) _baud * _ns_per_period));
    if (_cycles_per_bit < 2)
        _cycles_per_bit = 2;
    setParam("ns_per_period", std::to_string(_ns_per_period));
    setParam("baud", std::to_string(_baud));
    addInput("clk", 1, PinEdge::Rising);
    addOutput("rx", 1, false, true);
    // 构造期还没有并发访问者，普通赋值即可
    _state.head = 0;
    _state.tail = 0;
}

UartFeed::~UartFeed() = default;


std::vector<Component::JitSymbol> UartFeed::extraJitSymbols() const {
    // 只有标志置位时 JIT 才会调用它，正常仿真里这条路径几乎不触发
    return {{"_dcs_feed_pop", reinterpret_cast<void *>(&UartFeed::feedPop)}};
}

std::string UartFeed::genStateDecl() const {
    return std::format(
            "static uint32_t _feed_cnt_{0}, _feed_bit_{0};\n"
            "static uint8_t  _feed_busy_{0}, _feed_ch_{0};\n"
            "static unsigned int *_feed_head_{0} = (unsigned int *)0x{1:X}ULL;\n"
            "static unsigned int *_feed_tail_{0} = (unsigned int *)0x{2:X}ULL;\n",
            _id, reinterpret_cast<uintptr_t>(&_state.head), reinterpret_cast<uintptr_t>(&_state.tail));
}

std::string UartFeed::genInitCode() const {
    return std::format("    _feed_cnt_{0} = 0; _feed_bit_{0} = 0; _feed_busy_{0} = 0; _feed_ch_{0} = 0;\n", _id);
}

std::string UartFeed::genFuncDef_seq() const {
    std::string c;
    c += std::format("// UartFeed {}\n", cgen::commentText(name()));
    c += "extern int _dcs_feed_pop(void *);\n";
    c += std::format("static void {}() {{\n", funcName_seq());
    // 位时间按 clk 上升沿计数；tick 只是仿真步长
    c += "    const bool _clk_pos = " + genGetWireEdge(0, PinEdge::Rising) + ";\n";
    c += "    if (!_clk_pos) return;\n";
    // 队列非空才回调；head/tail 是原子量，生产者 push 后一定能在这里看到，
    // 不存在"清标志"把新数据挡在外面
    c += std::format(
            "    if (!_feed_busy_{0} && dcs_load_acq(_feed_head_{0}) != dcs_load_acq(_feed_tail_{0})) {{\n"
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
            _id, reinterpret_cast<uintptr_t>(&_state), _cycles_per_bit);
    c += "    " + genOutputWrite(0, "_level", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> UartFeed::clone(const std::string &new_name) const {
    return std::make_unique<UartFeed>(new_name, _ns_per_period, _baud);
}

bool UartFeed::push(uint8_t b) {
    // 允许两个宿主线程同时喂（stdin + --watch 文件），队列的 head/tail
    // 本身是 SPSC，多生产者必须在这里串行化
    std::lock_guard<std::mutex> lock(_push_mutex);
    std::atomic_ref<uint32_t> head(_state.head);
    const uint32_t h = head.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1) % CAP;
    // 队列满：返回 false 让生产者自己决定重试/丢弃，不能悄悄覆盖还没发出去的数据
    if (next == std::atomic_ref<uint32_t>(_state.tail).load(std::memory_order_acquire))
        return false;
    _state.buf[h] = b;
    head.store(next, std::memory_order_release);
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
    // atomic_ref 只能绑非 const 对象；pending 只读，去掉 const 只是为了让负载走原子读
    const uint32_t h = std::atomic_ref<uint32_t>(const_cast<uint32_t &>(_state.head)).load(std::memory_order_acquire);
    const uint32_t t = std::atomic_ref<uint32_t>(const_cast<uint32_t &>(_state.tail)).load(std::memory_order_acquire);
    return (h + CAP - t) % CAP;
}

int UartFeed::feedPop(void *state_ptr) {
    auto &s = *static_cast<FeedState *>(state_ptr);
    std::atomic_ref<uint32_t> tail(s.tail);
    const uint32_t t = tail.load(std::memory_order_relaxed);
    if (t == std::atomic_ref<uint32_t>(s.head).load(std::memory_order_acquire))
        return -1;
    const uint8_t ch = s.buf[t];
    tail.store((t + 1) % CAP, std::memory_order_release);
    return ch;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"uart_feed", {{"ns_per_period", "20"}, {"baud", "115200"}}, {"clk"}, {"rx"}, dsc::ComponentCategory::Misc, "串口输入端（外部喂数据）：按 ns_per_period/baud 算 8N1 位时间"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UartFeed>(n, dsc::paramInt(p, "ns_per_period", 20),
                                                    (std::uint64_t) dsc::paramInt(p, "baud", 115200));
        });
} // namespace
