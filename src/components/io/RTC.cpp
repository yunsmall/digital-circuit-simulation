#include "dcs/ComponentFactory.h"
#include "dcs/components/io/RTC.h"

#include <format>
#include <stdexcept>

namespace dsc {

namespace {
constexpr int I_CLK = 0, I_ADDR = 1, I_WDAT = 2, I_WE = 3, I_EN = 4;
constexpr int O_RDAT = 0, O_READY = 1, O_IRQ = 2;
} // namespace

RTC::RTC(const std::string &name, uint32_t ns_per_tick) : SequentialComponent(name, "rtc"), _ns_per_tick(ns_per_tick) {
    // 0 表示时间永不推进，和 WallClock 一样直接拒绝，不再静默改成 100
    if (_ns_per_tick == 0)
        throw std::invalid_argument("ns_per_tick 必须大于 0");
    setParam("ns_per_tick", std::to_string(_ns_per_tick));
    addInput("clk", 1);
    addInput("addr", 8);
    addInput("wdata", 32);
    addInput("we", 1);
    addInput("en", 1);
    addOutput("rdata", 32, false, true);
    addOutput("ready", 1, false, true);
    addOutput("irq", 1, false, true);
}

RTC::~RTC() = default;

std::string RTC::genStateDecl() const {
    // 时间基准直接编进常量：setBaseNs 必须在 compile 前调用。
    // 寄存器偏移用宏（嵌实例 id，避免多个实例宏名冲突）
    return std::format(
            "#define _RTC_{0}_TIME_LO 0x00u\n"
            "#define _RTC_{0}_TIME_HI 0x04u\n"
            "#define _RTC_{0}_ALARM_LO 0x08u\n"
            "#define _RTC_{0}_ALARM_HI 0x0Cu\n"
            "#define _RTC_{0}_IRQ_EN 0x10u\n"
            "#define _RTC_{0}_CLEAR 0x14u\n"
            "static uint64_t _rtc_now_{0};\n"
            "static uint64_t _rtc_alarm_{0};\n"
            "static uint32_t _rtc_irqen_{0};\n"
            "static uint8_t _rtc_prev_en_{0};\n"
            "static const uint64_t _rtc_base_{0} = 0x{1:X}ULL;\n"
            "static const uint32_t _rtc_nspt_{0} = {2};\n",
            _id, _base_ns, _ns_per_tick);
}

std::string RTC::genInitCode() const {
    // 闹钟初值必须是 0 而不是 ~0：写 ALARM_LOW 时高半要保留，初值 ~0 会让
    // 闹钟永远停在 0xFFFFFFFF_xxxxxxxx，怎么等都不到期
    return std::format(
            "    _rtc_now_{0} = _rtc_base_{0}; _rtc_alarm_{0} = 0; _rtc_irqen_{0} = 0;\n"
            "    _rtc_prev_en_{0} = 0;\n",
            _id);
}

std::string RTC::genFuncDef_seq() const {
    auto nid = [&](int i) { return std::to_string(inputs()[i]->netId()); };
    std::string c;
    c += std::format("// RTC {}\n", cgen::commentText(name()));
    c += std::format("static void {}() {{\n", funcName_seq());
    c += "    uint32_t _addr = 0; dcs_memcpy(&_addr, _w[" + nid(I_ADDR) + "], 1);\n";
    c += "    uint32_t _wdat = 0; dcs_memcpy(&_wdat, _w[" + nid(I_WDAT) + "], 4);\n";
    c += "    uint8_t _we = _w[" + nid(I_WE) + "][0] & 1;\n";
    c += "    uint8_t _en = _w[" + nid(I_EN) + "][0] & 1;\n";
    c += "    uint32_t _rd = 0; uint8_t _ready = 0;\n";
    // 每个 tick 推进固定时长，等价于每个时钟周期
    c += std::format("    _rtc_now_{0} += _rtc_nspt_{0};\n", _id);
    c += std::format(
            "    const bool _wr = _en && _we && !_rtc_prev_en_{0};\n"
            "    _rtc_prev_en_{0} = _en;\n"
            "    if (_wr) {{\n"
            "        switch (_addr) {{\n"
            "        case _RTC_{0}_ALARM_LO: _rtc_alarm_{0} = (_rtc_alarm_{0} & 0xFFFFFFFF00000000ULL) | (uint64_t)_wdat; break;\n"
            "        case _RTC_{0}_ALARM_HI: _rtc_alarm_{0} = (_rtc_alarm_{0} & 0xFFFFFFFFULL) | ((uint64_t)_wdat << 32); break;\n"
            "        case _RTC_{0}_IRQ_EN: _rtc_irqen_{0} = _wdat & 1u; break;\n"
            "        case _RTC_{0}_CLEAR: _rtc_irqen_{0} = 0; _rtc_alarm_{0} = 0; break;\n"
            "        default: break;\n"
            "        }}\n"
            "    }}\n"
            "    switch (_addr) {{\n"
            "    case _RTC_{0}_TIME_LO: _rd = (uint32_t)_rtc_now_{0}; break;\n"
            "    case _RTC_{0}_TIME_HI: _rd = (uint32_t)(_rtc_now_{0} >> 32); break;\n"
            "    case _RTC_{0}_ALARM_LO: _rd = (uint32_t)_rtc_alarm_{0}; break;\n"
            "    case _RTC_{0}_ALARM_HI: _rd = (uint32_t)(_rtc_alarm_{0} >> 32); break;\n"
            "    case _RTC_{0}_IRQ_EN: _rd = _rtc_irqen_{0}; break;\n"
            "    default: _rd = 0; break;\n"
            "    }}\n"
            "    _ready = _en;\n"
            "    uint8_t _irq = (_rtc_irqen_{0} && _rtc_now_{0} >= _rtc_alarm_{0}) ? 1 : 0;\n",
            _id);
    c += "    " + genOutputWrite(O_RDAT, "_rd", 32) + "\n";
    c += "    " + genOutputWrite(O_READY, "_ready", 1) + "\n";
    c += "    " + genOutputWrite(O_IRQ, "_irq", 1) + "\n";
    c += "}\n";
    return c;
}

std::unique_ptr<Component> RTC::clone(const std::string &new_name) const {
    auto r = std::make_unique<RTC>(new_name, _ns_per_tick);
    // 时间基准是配置而不是运行期状态，不复制会让 clone 出来的 RTC 从 epoch 开始
    r->setBaseNs(_base_ns);
    return r;
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"rtc", {{"ns_per_tick", "100"}}, {"clk", "addr", "wdata", "we", "en"}, {"rdata", "ready", "irq"}, dsc::ComponentCategory::Misc, "goldfish 风格实时时钟：时间寄存器单位纳秒，带闹钟中断"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
            const int npt = dsc::paramInt(p, "ns_per_tick", 100);
            // 负值强转 uint32 会变成 40 多亿 ns/tick，RTC 看起来正常却能跳出天文数字；
            // 和 WallClock 用同一范围和同一语义（0 也拒绝）
            if (npt < 1 || npt > 1000000000)
                throw std::invalid_argument("ns_per_tick 必须在1-1000000000之间");
            return std::make_unique<dsc::RTC>(n, (uint32_t) npt);
        });
} // namespace
