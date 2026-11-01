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
    _state.head = _state.tail = 0;
    _state.has_data = 0;
}

UartIn::~UartIn() = default;

bool UartIn::prepare(std::string &) { return true; }

std::vector<Component::JitSymbol> UartIn::extraJitSymbols() const {
    return {{"_dcs_uart_in_fetch", reinterpret_cast<void *>(&UartIn::fetch)}};
}

std::string UartIn::genStateDecl() const {
    return std::format(
            "static uint32_t _uin_cnt_{0}, _uin_bit_{0}, _uin_poll_{0}, _uin_ch_{0};\n"
            "static uint8_t  _uin_busy_{0};\n"
            "static volatile uint32_t *_uin_flag_{0} = (volatile uint32_t *)0x{1:X}ULL;\n",
            _id, reinterpret_cast<uintptr_t>(&_state.has_data));
}

std::string UartIn::genInitCode() const {
    return std::format(
            "    _uin_cnt_{0} = 0; _uin_bit_{0} = 0; _uin_poll_{0} = 0; _uin_ch_{0} = 0;\n"
            "    _uin_busy_{0} = 0;\n",
            _id);
}

std::string UartIn::genFuncDef_seq() const {
    std::string c;
    c += std::format("// UartIn {}\n", name());
    c += "extern int _dcs_uart_in_fetch(void *, int);\n";
    c += std::format("static void {}() {{\n", funcName_seq());
    c += std::format(
            "    if (!_uin_busy_{0}) {{\n"
            "        int _c = -1;\n"
            // 队列里有数据（外部 push）就走这条路，不必等轮询周期
            "        if (*_uin_flag_{0}) {{\n"
            "            _c = _dcs_uart_in_fetch((void *)0x{1:X}ULL, 0);\n"
            "            _uin_poll_{0} = 0;\n"
            "        }} else if (++_uin_poll_{0} >= {2}u) {{\n"
            // 每次问 stdin 都是一次系统调用，逐拍问会把仿真拖慢几个数量级
            "            _uin_poll_{0} = 0;\n"
            "            _c = _dcs_uart_in_fetch((void *)0x{1:X}ULL, 1);\n"
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
            "        if (++_uin_cnt_{0} >= {3}u) {{\n"
            "            _uin_cnt_{0} = 0;\n"
            "            if (++_uin_bit_{0} > 9) _uin_busy_{0} = 0;\n"
            "        }}\n"
            "    }}\n",
            _id, reinterpret_cast<uintptr_t>(&_state), InState::POLL_INTERVAL, _baud_div);
    c += "    " + genOutputWrite(0, "_level", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> UartIn::clone(const std::string &new_name) const {
    return std::make_unique<UartIn>(new_name, _baud_div);
}

void UartIn::pushByte(uint8_t b) {
    const uint8_t next = (uint8_t) ((_state.head + 1) & 63);
    if (next == _state.tail)
        return; // 队列满，丢弃
    _state.fifo[_state.head & 63] = b;
    _state.head = next;
    _state.has_data = 1;
}

size_t UartIn::pending() const { return (uint8_t) (_state.head - _state.tail); }

// 返回下一个要发送的字节，没有则 -1。do_poll=1 时才允许去碰 stdin
int UartIn::fetch(void *state_ptr, int do_poll) {
    auto &s = *static_cast<InState *>(state_ptr);
    if (s.head != s.tail) {
        const uint8_t b = s.fifo[s.tail & 63];
        s.tail = (uint8_t) ((s.tail + 1) & 63);
        if (s.head == s.tail)
            s.has_data = 0; // 队列空了，JIT 下一拍就不必再进来
        return b;
    }
    s.has_data = 0;
    return do_poll ? pollStdin() : -1;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"uart_in", {{"baud_div", "16"}}, {"clk"}, {"rx"}, dsc::ComponentCategory::Misc, "串口输入端：把宿主 stdin 的字符按 8N1 发到 UART 的 rx 引脚（baud_div 要和 UART 一致）"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            return std::make_unique<dsc::UartIn>(n, dsc::paramInt(p, "baud_div", 16));
        });
} // namespace
