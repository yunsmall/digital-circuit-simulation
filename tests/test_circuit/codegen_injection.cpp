// 生成 C 的注入防护：JSON 里的元件名不能逃出注释或续行吞掉生成的代码
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <dcs/Circuit.h>
#include <dcs/CodeGenText.h>
#include <dcs/components/io/Gpio.h>

using namespace dsc;

namespace {

// Gpio 的生成代码会按引脚读线网，输入必须先接上线网才能编译
Gpio *addConnectedGpio(Circuit &c, const std::string &name) {
    auto *g = static_cast<Gpio *>(c.addComponent(std::make_unique<Gpio>(name)));
    if (!g)
        return nullptr;
    Net *bus = c.createNet("bus");
    for (const char *pin: {"clk", "addr", "wdata", "we", "en", "gpio_in"})
        c.connect(g, pin, bus);
    return g;
}

} // namespace

TEST(CodeGenText, CommentTextRemovesLineBreaks) {
    const std::string safe = cgen::commentText("a\nb\r\nc");
    EXPECT_EQ(safe.find('\n'), std::string::npos);
    EXPECT_EQ(safe.find('\r'), std::string::npos);
    EXPECT_NE(safe.find('a'), std::string::npos);
    EXPECT_NE(safe.find('b'), std::string::npos);
    EXPECT_NE(safe.find('c'), std::string::npos);
}

// 反斜杠、分号、括号、引号、斜杠、问号等能改变 C 语法的字符一律变下划线
TEST(CodeGenText, DangerousCharactersBecomeUnderscore) {
    EXPECT_EQ(cgen::commentText("a\\b"), "a_b");
    EXPECT_EQ(cgen::commentText("a;b"), "a_b");
    EXPECT_EQ(cgen::commentText("a(b)"), "a_b_");
    EXPECT_EQ(cgen::commentText("a\"b'c"), "a_b_c");
    EXPECT_EQ(cgen::commentText("a/b"), "a_b");
    EXPECT_EQ(cgen::commentText("a?b"), "a_b");
    EXPECT_EQ(cgen::commentText("a{}b"), "a__b");
    EXPECT_EQ(cgen::commentText("x\\").back(), '_');
}

TEST(CodeGenText, KeepsReadableCharacters) {
    EXPECT_EQ(cgen::commentText("U1: cpu-0.v_2"), "U1: cpu-0.v_2");
    EXPECT_EQ(cgen::commentText("时钟 1"), "时钟 1");
}

TEST(CodeGenInjection, NewlineInNameCannotStartNewLineOfCode) {
    Circuit c;
    auto *g = addConnectedGpio(c, "g\nvoid dcs_injected(void) {}\n//");
    ASSERT_NE(g, nullptr);

    auto err = c.compile();
    ASSERT_EQ(err.code, ErrorCode::OK) << err.message;
    const std::string code = c.generateC();
    // 名字允许出现在注释里，但不能在换行后形成一条真正的函数定义
    EXPECT_EQ(code.find("\nvoid dcs_injected"), std::string::npos);
    EXPECT_EQ(code.find("\n//\n"), std::string::npos);
}

TEST(CodeGenInjection, TrailingBackslashInNameDoesNotSpliceLines) {
    Circuit c;
    auto *g = addConnectedGpio(c, "g\\");
    ASSERT_NE(g, nullptr);
    auto err = c.compile();
    EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
}

TEST(CodeGenInjection, TrigraphLikeEndingDoesNotSpliceLines) {
    Circuit c;
    auto *g = addConnectedGpio(c, std::string("g??") + "/");
    ASSERT_NE(g, nullptr);
    auto err = c.compile();
    EXPECT_EQ(err.code, ErrorCode::OK) << err.message;
}
