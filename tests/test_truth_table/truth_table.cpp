// 真值表 / 卡诺图：组合枚举顺序、格式化、参数校验
#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/TruthTable.h>
#include <dcs/components/combinational/Gates.h>

using namespace dsc;

namespace {

void add2x1(Circuit &c, GateOp op, Net *&a, Net *&b, Net *&y) {
    auto g = std::make_unique<LogicGate>("g0", 2, 1, op);
    auto *gp = g.get();
    c.addComponent(std::move(g));
    a = c.createNet("a");
    b = c.createNet("b");
    y = c.createNet("y");
    c.connect(gp, "in0", a);
    c.connect(gp, "in1", b);
    c.connect(gp, "out", y);
}

void mustCompile(Circuit &c) {
    auto err = c.compile();
    if (err.code != ErrorCode::OK)
        throw std::runtime_error(err.message);
}

} // namespace

TEST(TruthTable, XorOrdering) {
    Circuit c;
    Net *a, *b, *y;
    add2x1(c, GateOp::XOR, a, b, y);
    mustCompile(c);

    TruthTable t;
    std::string err;
    ASSERT_TRUE(computeTruthTable(c, {"a", "b"}, "y", t, err)) << err;
    ASSERT_EQ(t.values.size(), 4u);
    // a 是最高位：00,01,10,11
    EXPECT_EQ(t.values[0], 0);
    EXPECT_EQ(t.values[1], 1);
    EXPECT_EQ(t.values[2], 1);
    EXPECT_EQ(t.values[3], 0);
}

TEST(TruthTable, AndThreeVariables) {
    Circuit c;
    // y = a & (b | c)
    auto orb = std::make_unique<LogicGate>("orb", 2, 1, GateOp::OR);
    auto *orbp = orb.get();
    c.addComponent(std::move(orb));
    auto andg = std::make_unique<LogicGate>("andg", 2, 1, GateOp::AND);
    auto *andp = andg.get();
    c.addComponent(std::move(andg));

    Net *a = c.createNet("a");
    Net *b = c.createNet("b");
    Net *cc = c.createNet("c");
    Net *t = c.createNet("t");
    Net *y = c.createNet("y");
    c.connect(orbp, "in0", b);
    c.connect(orbp, "in1", cc);
    c.connect(orbp, "out", t);
    c.connect(andp, "in0", a);
    c.connect(andp, "in1", t);
    c.connect(andp, "out", y);
    mustCompile(c);

    TruthTable t3;
    std::string err;
    ASSERT_TRUE(computeTruthTable(c, {"a", "b", "c"}, "y", t3, err)) << err;
    ASSERT_EQ(t3.values.size(), 8u);
    const uint8_t expect[8] = {0, 0, 0, 0, 0, 1, 1, 1};
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(t3.values[i], expect[i]) << "combo " << i;
}

TEST(TruthTable, FormatsTextTable) {
    Circuit c;
    Net *a, *b, *y;
    add2x1(c, GateOp::AND, a, b, y);
    mustCompile(c);
    TruthTable t;
    std::string err;
    ASSERT_TRUE(computeTruthTable(c, {"a", "b"}, "y", t, err));
    const std::string s = formatTruthTable(t);
    EXPECT_NE(s.find("a b | y"), std::string::npos);
    EXPECT_NE(s.find("1 1 | 1"), std::string::npos);
    EXPECT_NE(s.find("0 0 | 0"), std::string::npos);
}

TEST(TruthTable, KarnaughGrid) {
    Circuit c;
    Net *a, *b, *y;
    add2x1(c, GateOp::AND, a, b, y);
    mustCompile(c);
    TruthTable t;
    std::string err;
    ASSERT_TRUE(computeTruthTable(c, {"a", "b"}, "y", t, err));
    const std::string k = formatKarnaugh(t);
    EXPECT_NE(k.find("b"), std::string::npos) << k;
    // 格雷码列顺序 0、1；行 a=0 那一行两个 0，a=1 那一行 0、1
    EXPECT_NE(k.find("a=0"), std::string::npos) << k;
    EXPECT_NE(k.find("a=1"), std::string::npos) << k;
}

TEST(TruthTable, KarnaughGrayOrderFourVars) {
    Circuit c;
    // y = a & b & c & d 用两级 2 输入 AND 拼
    auto g1 = std::make_unique<LogicGate>("g1", 2, 1, GateOp::AND);
    auto g2 = std::make_unique<LogicGate>("g2", 2, 1, GateOp::AND);
    auto g3 = std::make_unique<LogicGate>("g3", 2, 1, GateOp::AND);
    auto *p1 = g1.get();
    auto *p2 = g2.get();
    auto *p3 = g3.get();
    c.addComponent(std::move(g1));
    c.addComponent(std::move(g2));
    c.addComponent(std::move(g3));
    Net *a = c.createNet("a"), *b = c.createNet("b"), *cc = c.createNet("c"), *d = c.createNet("d");
    Net *t1 = c.createNet("t1"), *t2 = c.createNet("t2"), *y = c.createNet("y");
    c.connect(p1, "in0", a);
    c.connect(p1, "in1", b);
    c.connect(p1, "out", t1);
    c.connect(p2, "in0", cc);
    c.connect(p2, "in1", d);
    c.connect(p2, "out", t2);
    c.connect(p3, "in0", t1);
    c.connect(p3, "in1", t2);
    c.connect(p3, "out", y);
    mustCompile(c);

    TruthTable t;
    std::string err;
    ASSERT_TRUE(computeTruthTable(c, {"a", "b", "c", "d"}, "y", t, err));
    // 只有 1111 为 1
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(t.values[i], i == 15 ? 1 : 0);
    const std::string k = formatKarnaugh(t);
    // 行标签是 ab，列标签是 cd，格雷码顺序 00 01 11 10
    EXPECT_NE(k.find("ab=11"), std::string::npos) << k;
    EXPECT_NE(k.find(" 11"), std::string::npos) << k;
    EXPECT_NE(k.find(" 10"), std::string::npos) << k;
}

TEST(TruthTable, TooManyVarsForKarnaugh) {
    Circuit c;
    auto g = std::make_unique<LogicGate>("g0", 7, 1, GateOp::OR);
    auto *gp = g.get();
    c.addComponent(std::move(g));
    std::vector<Net *> ins;
    for (int i = 0; i < 7; i++) {
        ins.push_back(c.createNet(std::format("x{}", i)));
        c.connect(gp, std::format("in{}", i), ins.back());
    }
    Net *y = c.createNet("y");
    c.connect(gp, "out", y);
    mustCompile(c);

    std::vector<std::string> names;
    for (int i = 0; i < 7; i++)
        names.push_back(std::format("x{}", i));
    TruthTable t;
    std::string err;
    ASSERT_TRUE(computeTruthTable(c, names, "y", t, err));
    EXPECT_EQ(t.values.size(), 128u);
    const std::string k = formatKarnaugh(t);
    EXPECT_NE(k.find("2~6"), std::string::npos) << k;
}

TEST(TruthTable, RejectsMissingNets) {
    Circuit c;
    Net *a, *b, *y;
    add2x1(c, GateOp::AND, a, b, y);
    mustCompile(c);
    TruthTable t;
    std::string err;
    EXPECT_FALSE(computeTruthTable(c, {"a", "nope"}, "y", t, err));
    EXPECT_NE(err.find("nope"), std::string::npos);
    EXPECT_FALSE(computeTruthTable(c, {"a", "b"}, "nope", t, err));
}

TEST(TruthTable, RejectsWideNets) {
    Circuit c;
    // 2 位输入的与门：网位宽变成 2
    auto g = std::make_unique<LogicGate>("g0", 2, 2, GateOp::AND);
    auto *gp = g.get();
    c.addComponent(std::move(g));
    Net *a = c.createNet("a");
    Net *b = c.createNet("b");
    Net *y = c.createNet("y");
    c.connect(gp, "in0", a);
    c.connect(gp, "in1", b);
    c.connect(gp, "out", y);
    mustCompile(c);

    TruthTable t;
    std::string err;
    EXPECT_FALSE(computeTruthTable(c, {"a", "b"}, "y", t, err));
    EXPECT_NE(err.find("1 位"), std::string::npos);
}

TEST(TruthTable, RejectsEmptyInputs) {
    Circuit c;
    Net *a, *b, *y;
    add2x1(c, GateOp::AND, a, b, y);
    mustCompile(c);
    TruthTable t;
    std::string err;
    EXPECT_FALSE(computeTruthTable(c, {}, "y", t, err));
}
