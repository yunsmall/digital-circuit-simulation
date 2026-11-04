// WallClock：只在时钟上升沿推进；公历换算、星期、闰日、跨日/跨年翻转
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include <dcs/Circuit.h>
#include <dcs/components/clock/WallClock.h>

using namespace dsc;

namespace {

struct Fx {
    std::unique_ptr<Circuit> c = std::make_unique<Circuit>();
    WallClock *w = nullptr;
    Net *clk, *year, *month, *day, *hour, *minute, *second, *weekday;

    Fx(int ns_per_tick, const std::string &base, bool sync_host = false) {
        auto p = std::make_unique<WallClock>("wc0", ns_per_tick, base, sync_host);
        w = p.get();
        c->addComponent(std::move(p));
        clk = c->createNet("clk");
        year = c->createNet("year");
        month = c->createNet("month");
        day = c->createNet("day");
        hour = c->createNet("hour");
        minute = c->createNet("minute");
        second = c->createNet("second");
        weekday = c->createNet("weekday");
        c->connect(w, "clk", clk);
        c->connect(w, "year", year);
        c->connect(w, "month", month);
        c->connect(w, "day", day);
        c->connect(w, "hour", hour);
        c->connect(w, "minute", minute);
        c->connect(w, "second", second);
        c->connect(w, "weekday", weekday);
        auto err = c->compile();
        if (err.code != ErrorCode::OK)
            throw std::runtime_error(err.message);
        c->init();
        c->setWire(clk->id(), 0);
    }

    uint32_t v(Net *n) const {
        auto d = c->getWire(n->id());
        return d.empty() ? 0u : (uint32_t) d[0] | ((uint32_t) (d.size() > 1 ? d[1] : 0) << 8);
    }

    // cycles 个完整时钟周期（低 → 高 → 低），每个上升沿推进一次时间
    void cycles(int n = 1) {
        for (int i = 0; i < n; i++) {
            c->setWire(clk->id(), 0);
            c->tick();
            c->setWire(clk->id(), 1);
            c->tick();
            c->setWire(clk->id(), 0);
            c->tick();
        }
    }

    // 不产生时钟沿
    void hold(int n) {
        for (int i = 0; i < n; i++) {
            c->setWire(clk->id(), 0);
            c->tick();
        }
    }
};

} // namespace

TEST(WallClock, EpochIsThursday1970) {
    Fx f(1, "0");
    f.cycles();
    EXPECT_EQ(f.v(f.year), 1970u);
    EXPECT_EQ(f.v(f.month), 1u);
    EXPECT_EQ(f.v(f.day), 1u);
    EXPECT_EQ(f.v(f.hour), 0u);
    EXPECT_EQ(f.v(f.minute), 0u);
    EXPECT_EQ(f.v(f.second), 0u);
    EXPECT_EQ(f.v(f.weekday), 4u); // 周四
}

TEST(WallClock, Year2000) {
    Fx f(1, "946684800");
    f.cycles();
    EXPECT_EQ(f.v(f.year), 2000u);
    EXPECT_EQ(f.v(f.month), 1u);
    EXPECT_EQ(f.v(f.day), 1u);
    EXPECT_EQ(f.v(f.weekday), 6u); // 周六
}

TEST(WallClock, LeapDay2024) {
    Fx f(1, "1709164800");
    f.cycles();
    EXPECT_EQ(f.v(f.year), 2024u);
    EXPECT_EQ(f.v(f.month), 2u);
    EXPECT_EQ(f.v(f.day), 29u);
    EXPECT_EQ(f.v(f.weekday), 4u); // 周四
}

TEST(WallClock, TimeOfDay) {
    Fx f(1, "1793536496"); // 2026-11-01 12:34:56
    f.cycles();
    EXPECT_EQ(f.v(f.year), 2026u);
    EXPECT_EQ(f.v(f.month), 11u);
    EXPECT_EQ(f.v(f.day), 1u);
    EXPECT_EQ(f.v(f.hour), 12u);
    EXPECT_EQ(f.v(f.minute), 34u);
    EXPECT_EQ(f.v(f.second), 56u);
    EXPECT_EQ(f.v(f.weekday), 0u); // 周日
}

TEST(WallClock, AdvancesOneSecondPerGigaTickEdge) {
    Fx f(1000000000, "1793536496"); // 每个上升沿前进 1 秒
    f.cycles();
    EXPECT_EQ(f.v(f.second), 57u);
    f.cycles();
    EXPECT_EQ(f.v(f.second), 58u);
}

TEST(WallClock, DoesNotAdvanceWithoutClockEdge) {
    Fx f(1000000000, "1793536496");
    f.cycles(); // 先来一个沿，时间 = base + 1 秒
    const uint32_t s = f.v(f.second);
    f.hold(20);
    EXPECT_EQ(f.v(f.second), s);
    EXPECT_EQ(f.v(f.minute), 34u);
}

TEST(WallClock, RollsOverMidnight) {
    Fx f(1000000000, "1704067199"); // 2023-12-31 23:59:59
    f.cycles();
    EXPECT_EQ(f.v(f.year), 2024u);
    EXPECT_EQ(f.v(f.month), 1u);
    EXPECT_EQ(f.v(f.day), 1u);
    EXPECT_EQ(f.v(f.hour), 0u);
    EXPECT_EQ(f.v(f.minute), 0u);
    EXPECT_EQ(f.v(f.second), 0u);
    EXPECT_EQ(f.v(f.weekday), 1u); // 2024-01-01 是周一
}

TEST(WallClock, SubSecondEdgesDoNotChangeSecond) {
    Fx f(1, "1793536496");
    for (int i = 0; i < 10; i++)
        f.cycles();
    EXPECT_EQ(f.v(f.second), 56u);
    EXPECT_EQ(f.v(f.minute), 34u);
}

TEST(WallClock, LeapDayTransition) {
    Fx f(1000000000, "1709251199"); // 2024-02-29 23:59:59
    f.cycles();
    EXPECT_EQ(f.v(f.month), 3u);
    EXPECT_EQ(f.v(f.day), 1u);
}

TEST(WallClock, SyncHostFollowsRealClock) {
    Fx f(1, "", true); // 每个沿直接取宿主当前时间
    f.cycles();
    EXPECT_GE(f.v(f.year), 2020u);
    EXPECT_LT(f.v(f.year), 2100u);
    EXPECT_GE(f.v(f.month), 1u);
    EXPECT_LE(f.v(f.month), 12u);
    EXPECT_GE(f.v(f.day), 1u);
    EXPECT_LE(f.v(f.day), 31u);
    EXPECT_LE(f.v(f.hour), 23u);
    EXPECT_LE(f.v(f.minute), 59u);
    EXPECT_LE(f.v(f.second), 59u);
    EXPECT_LE(f.v(f.weekday), 6u);
}

// base_epoch 会被拼进生成的 C，必须严格是全数字；分号/换行/符号一律拒绝
TEST(WallClock, RejectsNonNumericBaseEpoch) {
    EXPECT_THROW(WallClock("w", 100, "0; void pwn(){}", false), std::invalid_argument);
    EXPECT_THROW(WallClock("w", 100, "123abc", false), std::invalid_argument);
    EXPECT_THROW(WallClock("w", 100, " 1", false), std::invalid_argument);
    EXPECT_THROW(WallClock("w", 100, "-1", false), std::invalid_argument);
    EXPECT_THROW(WallClock("w", 100, "0\n1", false), std::invalid_argument);
}

TEST(WallClock, NormalizesBaseEpochInParams) {
    WallClock w("w", 100, "001700000000", false);
    EXPECT_EQ(w.exportParams().at("base_epoch"), "1700000000");
}

TEST(WallClock, JsonLoadRejectsInjectedBaseEpoch) {
    const char *json = R"json({"nets":[],"components":[
        {"type":"wallclock","name":"w","params":{"base_epoch":"0; pwn()"}}]})json";
    std::string error;
    auto c = Circuit::fromJson(json, error);
    EXPECT_EQ(c, nullptr);
    EXPECT_NE(error.find("base_epoch"), std::string::npos) << error;
}
