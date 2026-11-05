#include "dcs/ComponentFactory.h"
#include "dcs/components/clock/WallClock.h"

#include <charconv>
#include <chrono>
#include <format>
#include <stdexcept>
#include <system_error>

namespace dsc {

extern "C" unsigned long long dcs_wallclock_base(void) {
    using namespace std::chrono;
    return (unsigned long long) duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

WallClock::WallClock(const std::string &name, int ns_per_tick, const std::string &base_epoch, bool sync_host) :
    SequentialComponent(name, "wallclock"), _ns_per_tick(ns_per_tick), _base_epoch(base_epoch), _sync_host(sync_host) {
    if (ns_per_tick < 1 || ns_per_tick > 1000000000)
        throw std::invalid_argument("ns_per_tick 必须在1-1000000000之间");
    // base_epoch 会原样写进生成的 C（`<base> .base = <值>ULL;`），必须严格解析成
    // 无符号整数；只放行全数字，空串表示用宿主时间。任何分号/换行/运算符都不能进
    if (!_base_epoch.empty()) {
        uint64_t value = 0;
        const char *first = _base_epoch.data();
        const char *last = first + _base_epoch.size();
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || ptr != last)
            throw std::invalid_argument("base_epoch 必须是非负十进制整数（Unix 秒）");
        _base_epoch = std::to_string(value); // 归一化，避免前导零/异形数字进生成代码
    }
    setParam("ns_per_tick", std::to_string(ns_per_tick));
    setParam("base_epoch", _base_epoch);
    setParam("sync_host", sync_host ? "1" : "0");
    addInput("clk", 1);
    addOutput("year", 16, false, true);
    addOutput("month", 4, false, true);
    addOutput("day", 5, false, true);
    addOutput("hour", 5, false, true);
    addOutput("minute", 6, false, true);
    addOutput("second", 6, false, true);
    addOutput("weekday", 3, false, true);
}

std::string WallClock::genStructDef() const {
    return std::format(
            "typedef struct {{\n"
            "    uint64_t base, ns, last_sec;\n"
            "    uint32_t y, mo, d, h, mi, s, wd;\n"
            "    uint8_t prev_clk;\n"
            "}} {};",
            stateTypeName());
}

std::string WallClock::genStateDecl() const {
    return std::format("static {} {};\nextern unsigned long long dcs_wallclock_base(void);", stateTypeName(),
                       stateVarName());
}

std::string WallClock::genInitCode() const {
    auto st = stateVarName();
    std::string base;
    // 注意 genResetCode 默认复用本函数：空 base_epoch（用宿主时间）时 reset()
    // 会重新取一次宿主当前时间，模拟的是"设备重新上电"而不是回到 init 的基准
    if (_base_epoch.empty())
        base = std::format("    {}.base = dcs_wallclock_base();\n", st);
    else
        base = std::format("    {}.base = {}ULL;\n", st, _base_epoch);
    // last_sec 置成最大值，保证第一拍就把日历算出来
    return base + std::format(
                          "    {0}.ns = 0; {0}.last_sec = 0xFFFFFFFFFFFFFFFFULL; {0}.prev_clk = 0;\n"
                          "    {0}.y = 0; {0}.mo = 0; {0}.d = 0; {0}.h = 0; {0}.mi = 0; {0}.s = 0; {0}.wd = 0;\n",
                          st);
}

std::string WallClock::genFuncDef_seq() const {
    auto st = stateVarName();
    std::string c;
    c += std::format("static void {}() {{\n", funcName_seq());
    c += std::format("    bool _clk = false; dcs_memcpy(&_clk, _w[{}], 1);\n", inputs()[0]->netId());
    // 时间只在时钟上升沿推进；非沿拍只把锁存值输出，和真实时钟元件一致
    c += std::format("    if (_clk && !{}.prev_clk) {{\n", st);
    if (_sync_host) {
        // 每个沿直接问宿主当前时间，和真实时钟严格对齐，代价是一沿一次宿主调用
        c += "        const uint64_t _sec = dcs_wallclock_base();\n";
    } else {
        c += std::format("        {}.ns += {}ULL;\n", st, _ns_per_tick);
        c += std::format("        const uint64_t _sec = {}.base + {}.ns / 1000000000ULL;\n", st, st);
    }
    c += std::format("        if (_sec != {}.last_sec) {{\n", st);
    c += std::format("            {}.last_sec = _sec;\n", st);
    // 公历换算用 Howard Hinnant 的 civil_from_days，全是整数运算，TCC 也能直接译
    c += "            uint64_t _z = _sec / 86400ULL;\n";
    c += "            const uint64_t _rem = _sec % 86400ULL;\n";
    c += std::format("            {}.h = (uint32_t) (_rem / 3600ULL);\n", st);
    c += std::format("            {}.mi = (uint32_t) ((_rem / 60ULL) % 60ULL);\n", st);
    c += std::format("            {}.s = (uint32_t) (_rem % 60ULL);\n", st);
    // 1970-01-01 是星期四，所以偏移 4
    c += std::format("            {}.wd = (uint32_t) ((_z + 4ULL) % 7ULL);\n", st);
    c += "            _z += 719468ULL;\n";
    c += "            const uint64_t _era = _z / 146097ULL;\n";
    c += "            const uint32_t _doe = (uint32_t) (_z - _era * 146097ULL);\n";
    c += "            const uint32_t _yoe = (uint32_t) ((_doe - _doe / 1460u + _doe / 36524u - _doe / 146096u) / 365u);\n";
    c += "            const uint32_t _y = (uint32_t) (_yoe + _era * 400ULL);\n";
    c += "            const uint32_t _doy = _doe - (365u * _yoe + _yoe / 4u - _yoe / 100u);\n";
    c += "            const uint32_t _mp = (5u * _doy + 2u) / 153u;\n";
    c += std::format("            {}.d = (uint32_t) (_doy - (153u * _mp + 2u) / 5u + 1u);\n", st);
    c += std::format("            {}.mo = _mp < 10u ? _mp + 3u : _mp - 9u;\n", st);
    c += std::format("            {}.y = _y + ({}.mo <= 2u ? 1u : 0u);\n", st, st);
    c += "        }\n";
    c += "    }\n";
    c += std::format("    {}.prev_clk = _clk;\n", st);
    c += std::format("    uint32_t _o0 = {}.y;\n", st);
    c += "    " + genOutputWrite(0, "_o0", 16) + "\n";
    c += std::format("    uint32_t _o1 = {}.mo;\n", st);
    c += "    " + genOutputWrite(1, "_o1", 4) + "\n";
    c += std::format("    uint32_t _o2 = {}.d;\n", st);
    c += "    " + genOutputWrite(2, "_o2", 5) + "\n";
    c += std::format("    uint32_t _o3 = {}.h;\n", st);
    c += "    " + genOutputWrite(3, "_o3", 5) + "\n";
    c += std::format("    uint32_t _o4 = {}.mi;\n", st);
    c += "    " + genOutputWrite(4, "_o4", 6) + "\n";
    c += std::format("    uint32_t _o5 = {}.s;\n", st);
    c += "    " + genOutputWrite(5, "_o5", 6) + "\n";
    c += std::format("    uint32_t _o6 = {}.wd;\n", st);
    c += "    " + genOutputWrite(6, "_o6", 3) + "\n";
    c += "}\n";
    return c;
}

std::vector<Component::JitSymbol> WallClock::extraJitSymbols() const {
    return {{"dcs_wallclock_base", reinterpret_cast<void *>(&dcs_wallclock_base)}};
}

std::unique_ptr<Component> WallClock::clone(const std::string &n) const {
    return std::make_unique<WallClock>(n, _ns_per_tick, _base_epoch, _sync_host);
}

} // namespace dsc

namespace {
DCS_REGISTER_COMPONENT(
    {"wallclock", {{"ns_per_tick", "100"}, {"base_epoch", ""}, {"sync_host", "0", {"0", "1"}}}, {"clk"},
     {"year", "month", "day", "hour", "minute", "second", "weekday"}, dsc::ComponentCategory::Misc,
     "日历时钟：虚拟模式按 ns_per_tick 累加；sync_host=1 时每个上升沿直接取宿主当前时间"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::WallClock>(n, dsc::paramInt(p, "ns_per_tick", 100), dsc::paramStr(p, "base_epoch", ""),
                                                dsc::paramInt(p, "sync_host", 0) != 0);
    });
} // namespace
