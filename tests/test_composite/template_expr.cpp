// TemplateExpr：文法、优先级、拼接规则和各种错误边界
#include <dcs/util/TemplateExpr.h>
#include <gtest/gtest.h>

#include <string>

using dsc::TemplateExpr;

namespace {

using Params = TemplateExpr::Params;

std::string ok(const std::string &src, const Params &params) {
    TemplateExpr expr(src, params);
    std::string out;
    EXPECT_TRUE(expr.resolve(out)) << expr.error();
    return out;
}

struct Error {
    bool failed = false;
    std::string error;
    std::string missing;
};

Error err(const std::string &src, const Params &params) {
    TemplateExpr expr(src, params);
    std::string out;
    Error e;
    e.failed = !expr.resolve(out);
    e.error = expr.error();
    e.missing = expr.missingParam();
    return e;
}

} // namespace

TEST(TemplateExpr, LiteralTextIsUnchanged) {
    EXPECT_EQ(ok("plain text", {}), "plain text");
    EXPECT_EQ(ok("u_1", {}), "u_1");
}

TEST(TemplateExpr, EmptyExpressionIsRejected) {
    for (const char *src: {"${}", "${   }", "a${}b"}) {
        auto e = err(src, {});
        EXPECT_TRUE(e.failed) << src;
        EXPECT_FALSE(e.error.empty()) << src;
    }
}

// 没有右花括号时沿用老行为：整段当普通文本，不报错
TEST(TemplateExpr, UnterminatedStaysLiteral) {
    EXPECT_EQ(ok("a${b", {}), "a${b");
}

TEST(TemplateExpr, AdjacentSubstitutionsConcatenate) {
    const Params p{{"a", "x"}, {"b", "y"}};
    EXPECT_EQ(ok("${a}${b}", p), "xy");
    EXPECT_EQ(ok("u_${a}_${b}", p), "u_x_y");
}

TEST(TemplateExpr, PrecedenceAndAssociativity) {
    EXPECT_EQ(ok("${2+3*4}", {}), "14");
    EXPECT_EQ(ok("${(2+3)*4}", {}), "20");
    EXPECT_EQ(ok("${2*(3+4)*2}", {}), "28");
    EXPECT_EQ(ok("${10-3-2}", {}), "5");
    EXPECT_EQ(ok("${20/5/2}", {}), "2");
}

TEST(TemplateExpr, UnaryOperators) {
    const Params p{{"w", "4"}};
    EXPECT_EQ(ok("${-w}", p), "-4");
    EXPECT_EQ(ok("${+w}", p), "4");
    EXPECT_EQ(ok("${-(-w)}", p), "4");
    EXPECT_EQ(ok("${--w}", p), "4");
}

TEST(TemplateExpr, DivisionTruncatesTowardZero) {
    EXPECT_EQ(ok("${-7/2}", {}), "-3");
    EXPECT_EQ(ok("${7/-2}", {}), "-3");
    EXPECT_EQ(ok("${-7/-2}", {}), "3");
}

TEST(TemplateExpr, WhitespaceAroundExpressionIsIgnored) {
    const Params p{{"w", "4"}};
    EXPECT_EQ(ok("${  w  +  1  }", p), "5");
    EXPECT_EQ(ok("a ${ w + 1 } b", p), "a 5 b");
}

TEST(TemplateExpr, NumericAndStringParametersFollowTheirType) {
    const Params p{{"a", "10"}, {"b", "2"}, {"s", "x"}};
    EXPECT_EQ(ok("${a+b}", p), "12");
    EXPECT_EQ(ok("${a/b}", p), "5");
    EXPECT_EQ(ok("${s+a}", p), "x10");
    EXPECT_EQ(ok("${a+s}", p), "10x");
}

TEST(TemplateExpr, StandaloneParamKeepsOriginalText) {
    const Params p{{"v", "007"}};
    EXPECT_EQ(ok("${v}", p), "007");
    EXPECT_EQ(ok("${007}", {}), "007");
    EXPECT_EQ(ok("${007+1}", {}), "8") << "参与算术后按数值输出";
}

// 整个 ${...} 与参数名完全一致时优先取参数值，便于参数名本身带运算符
TEST(TemplateExpr, DirectParamNameWinsOverExpression) {
    const Params p{{"a+b", "literal"}, {"a", "1"}, {"b", "2"}};
    EXPECT_EQ(ok("${a+b}", p), "literal");
    EXPECT_EQ(ok("${ a+b }", p), "literal") << "允许前后空格";
}

TEST(TemplateExpr, IdentifierMayContainDot) {
    const Params p{{"foo.bar", "41"}};
    EXPECT_EQ(ok("${foo.bar}", p), "41");
    EXPECT_EQ(ok("${foo.bar+1}", p), "42");
}

TEST(TemplateExpr, EmptyParameterValue) {
    const Params p{{"e", ""}, {"x", "x"}};
    EXPECT_EQ(ok("${e}", p), "");
    EXPECT_EQ(ok("${e+x}", p), "x");
}

TEST(TemplateExpr, Int64Boundaries) {
    EXPECT_EQ(ok("${9223372036854775807}", {}), "9223372036854775807");
    EXPECT_EQ(ok("${-9223372036854775807-1}", {}), "-9223372036854775808");

    auto add = err("${9223372036854775807+1}", {});
    EXPECT_TRUE(add.failed);
    EXPECT_NE(add.error.find("整数溢出"), std::string::npos) << add.error;

    auto literal = err("${9223372036854775808}", {});
    EXPECT_TRUE(literal.failed);

    auto negate = err("${-(-9223372036854775807-1)}", {});
    EXPECT_TRUE(negate.failed);
    EXPECT_NE(negate.error.find("整数溢出"), std::string::npos) << negate.error;
}

TEST(TemplateExpr, SyntaxErrorsAreReported) {
    for (const char *src: {"${w%2}", "${(w+1}", "${w+}", "${w 1}", "${w(1)}"}) {
        auto e = err(src, {{"w", "4"}});
        EXPECT_TRUE(e.failed) << src;
        EXPECT_FALSE(e.error.empty()) << src;
    }
}

TEST(TemplateExpr, UnknownParameterInsideExpression) {
    auto e = err("${nope+1}", {});
    EXPECT_TRUE(e.failed);
    EXPECT_EQ(e.missing, "nope");
    EXPECT_NE(e.error.find("nope"), std::string::npos) << e.error;
}

TEST(TemplateExpr, OperatorsRejectStringOperands) {
    for (const char *src: {"${s*2}", "${s-1}", "${s/2}", "${-s}"}) {
        auto e = err(src, {{"s", "x"}});
        EXPECT_TRUE(e.failed) << src;
        EXPECT_FALSE(e.error.empty()) << src;
    }
}

TEST(TemplateExpr, DivisionByZeroIsRejected) {
    for (const char *src: {"${w/0}", "${w/(1-1)}"}) {
        auto e = err(src, {{"w", "4"}});
        EXPECT_TRUE(e.failed) << src;
        EXPECT_NE(e.error.find("除数为 0"), std::string::npos) << e.error;
    }
}

TEST(TemplateExpr, NegativeParameterValues) {
    const Params p{{"n", "-5"}};
    EXPECT_EQ(ok("${n}", p), "-5");
    EXPECT_EQ(ok("${n+1}", p), "-4");
    EXPECT_EQ(ok("${n*n}", p), "25");
}
