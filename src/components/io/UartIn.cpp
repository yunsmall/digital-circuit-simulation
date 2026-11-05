// UartIn — 宿主 stdin → 8N1 串行比特流
// 发送状态机展开在 JIT 里；取字节要碰 stdin（系统调用），只在需要时才回调，
// 空闲时每 POLL_INTERVAL 拍才问一次。
#include "dcs/ComponentFactory.h"
#include "dcs/components/io/UartIn.h"

#include <cstring>
#include <format>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

namespace dsc {

namespace {
// 非阻塞取一个输入字节，没输入返回 -1。
// 终端输入走 _kbhit/_getch（控制台句柄不能用 PeekNamedPipe），
// 管道/重定向走 PeekNamedPipe + ReadFile，这样 `echo ... | dcs` 也能喂字符
int pollStdin() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (h && h != INVALID_HANDLE_VALUE && PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
        if (avail == 0)
            return -1;
        char c = 0;
        DWORD n = 0;
        if (ReadFile(h, &c, 1, &n, nullptr) && n == 1)
            return (unsigned char) c;
        return -1;
    }
    if (_kbhit()) {
        int c = _getch();
        // 方向键等功能键返回 0 / 0xE0 前缀，后面还跟着一个扫描码，整个丢掉
        if (c == 0 || c == 0xE0)
            return -1;
        return c & 0xFF;
    }
    return -1;
#else
    // stdin 默认是阻塞的，直接 read 会让整个仿真停在这里等按键；
    // 先零超时 select，确认可读再读
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{0, 0};
    if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return -1;
    unsigned char c = 0;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    return n == 1 ? (int) c : -1;
#endif
}
} // namespace

UartIn::UartIn(const std::string &name, int baud_div) :
    SequentialComponent(name, "uart_in"), _baud_div(baud_div < 2 ? 2 : baud_div) {
    setParam("baud_div", std::to_string(_baud_div));
    addInput("clk", 1);
    addOutput("rx", 1, false, true);
    _state.head = 0;
    _state.tail = 0;
}

UartIn::~UartIn() = default;


std::vector<Component::JitSymbol> UartIn::extraJitSymbols() const {
    return {{"_dcs_uart_in_fetch", reinterpret_cast<void *>(&UartIn::fetch)}};
}

std::string UartIn::genStateDecl() const {
    return std::format(
            "static uint32_t _uin_cnt_{0}, _uin_bit_{0}, _uin_poll_{0}, _uin_ch_{0};\n"
            "static uint8_t  _uin_busy_{0};\n"
            "static unsigned int *_uin_head_{0} = (unsigned int *)0x{1:X}ULL;\n"
            "static unsigned int *_uin_tail_{0} = (unsigned int *)0x{2:X}ULL;\n",
            _id, reinterpret_cast<uintptr_t>(&_state.head), reinterpret_cast<uintptr_t>(&_state.tail));
}

std::string UartIn::genInitCode() const {
    return std::format(
            "    _uin_cnt_{0} = 0; _uin_bit_{0} = 0; _uin_poll_{0} = 0; _uin_ch_{0} = 0;\n"
            "    _uin_busy_{0} = 0;\n",
            _id);
}

std::string UartIn::genFuncDef_seq() const {
    std::string c;
    // 不把 name() 写进注释：名字来自 JSON，可以包含换行，拼进 // 注释后会逃逸成代码
    c += "// UartIn\n";
    c += "extern int _dcs_uart_in_fetch(void *, void *, int);\n";
    c += std::format("static void {}() {{\n", funcName_seq());
    c += std::format(
            "    if (!_uin_busy_{0}) {{\n"
            "        int _c = -1;\n"
            // 队列里有数据（外部 push）就走这条路，不必等轮询周期。
            // 直接比较原子 head/tail，不用易丢的"有数据"标志
            "        if (dcs_load_acq(_uin_head_{0}) != dcs_load_acq(_uin_tail_{0})) {{\n"
            "            _c = _dcs_uart_in_fetch((void *)0x{1:X}ULL, (void *)0x{2:X}ULL, 0);\n"
            "            _uin_poll_{0} = 0;\n"
            "        }} else if (++_uin_poll_{0} >= {3}u) {{\n"
            // 轮询输入源可能有系统调用（默认源就是 stdin），逐拍问会把仿真拖慢几个数量级
            "            _uin_poll_{0} = 0;\n"
            "            _c = _dcs_uart_in_fetch((void *)0x{1:X}ULL, (void *)0x{2:X}ULL, 1);\n"
            "        }}\n"
            "        if (_c >= 0) {{\n"
            "            _uin_ch_{0} = (uint32_t)(_c & 0xFF);\n"
            "            _uin_busy_{0} = 1; _uin_bit_{0} = 0; _uin_cnt_{0} = 0;\n"
            "        }}\n"
            "    }}\n"
            "    uint8_t _level = 1;\n"
            "    if (_uin_busy_{0}) {{\n"
            "        if (_uin_bit_{0} == 0) _level = 0;\n"
            "        else if (_uin_bit_{0} <= 8) _level = (uint8_t)((_uin_ch_{0} >> (_uin_bit_{0} - 1)) & 1u);\n"
            "        if (++_uin_cnt_{0} >= {4}u) {{\n"
            "            _uin_cnt_{0} = 0;\n"
            "            if (++_uin_bit_{0} > 9) _uin_busy_{0} = 0;\n"
            "        }}\n"
            "    }}\n",
            _id, reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(&_state), InState::POLL_INTERVAL,
            _baud_div);
    c += "    " + genOutputWrite(0, "_level", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> UartIn::clone(const std::string &new_name) const {
    return std::make_unique<UartIn>(new_name, _baud_div);
}

void UartIn::pushByte(uint8_t b) {
    std::atomic_ref<uint32_t> head(_state.head);
    std::atomic_ref<uint32_t> tail(_state.tail);
    const uint32_t h = head.load(std::memory_order_relaxed);
    const uint32_t next = (h + 1) & 63;
    if (next == tail.load(std::memory_order_acquire))
        return; // 队列满，丢弃
    _state.fifo[h & 63] = b;
    head.store(next, std::memory_order_release);
}

size_t UartIn::pending() const {
    // atomic_ref 只能绑到非 const 对象上；pending 逻辑上只读，这里去掉 const 只是为了
    // 让负载用原子方式读，不会写
    std::atomic_ref<uint32_t> head(const_cast<uint32_t &>(_state.head));
    std::atomic_ref<uint32_t> tail(const_cast<uint32_t &>(_state.tail));
    const uint32_t h = head.load(std::memory_order_acquire);
    const uint32_t t = tail.load(std::memory_order_acquire);
    return (h + 64 - t) & 63;
}

void UartIn::setSource(const Source &source, void *ctx) {
    _source = source;
    _source_ctx = ctx;
}

// 只在允许轮询的拍调用；绑定过自定义源就不再碰宿主终端
int UartIn::pollSource(int do_poll) {
    if (!do_poll)
        return -1;
    if (_source.poll)
        return _source.poll(_source_ctx, this);
    return pollStdin();
}

// 返回下一个要发送的字节，没有则 -1。do_poll=1 时才允许去碰输入源
int UartIn::fetch(void *owner, void *state_ptr, int do_poll) {
    auto &s = *static_cast<InState *>(state_ptr);
    std::atomic_ref<uint32_t> tail(s.tail);
    const uint32_t t = tail.load(std::memory_order_relaxed);
    if (t != std::atomic_ref<uint32_t>(s.head).load(std::memory_order_acquire)) {
        const uint8_t b = s.fifo[t & 63];
        tail.store((t + 1) & 63, std::memory_order_release);
        return b;
    }
    return static_cast<UartIn *>(owner)->pollSource(do_poll);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"uart_in", {{"baud_div", "8"}}, {"clk"}, {"rx"}, dsc::ComponentCategory::Misc, "串口输入端：把宿主输入的字符按 8N1 发到 UART 的 rx 引脚（baud_div 默认与 uart 一致；可按实例 setSource 接管输入）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UartIn>(n, dsc::paramInt(p, "baud_div", 8));
        });
} // namespace
