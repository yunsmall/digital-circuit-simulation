// --help 显示宽度折行的单元测试。这些函数只在 CLI 入口用，但折坏 UTF-8
// 或把选项名和描述粘连是用户直接看得见的错误，所以按边缘情况逐条覆盖。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <dcs/util/TextWrap.h>

namespace {

std::vector<std::string> splitLines(const std::string &s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c: s) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    // 末尾没有换行时也保留最后一段；有换行则末尾不产生空行
    if (!cur.empty())
        lines.push_back(cur);
    return lines;
}

// 逐码点校验：首字节声明的长度必须和实际可用字节一致，续字节必须是 10xxxxxx
bool isValidUtf8(const std::string &s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char) s[i];
        std::size_t len = 1;
        if (c < 0x80)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            return false;
        if (i + len > s.size())
            return false;
        for (std::size_t k = 1; k < len; k++)
            if (((unsigned char) s[i + k] & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

std::size_t leadingSpaces(const std::string &s) {
    std::size_t n = 0;
    while (n < s.size() && s[n] == ' ')
        n++;
    return n;
}

} // namespace

// ---- displayWidth ----

TEST(DisplayWidth, Ascii) {
    EXPECT_EQ(dsc::displayWidth(""), 0u);
    EXPECT_EQ(dsc::displayWidth("abc"), 3u);
}

TEST(DisplayWidth, CjkCountsTwoColumns) {
    EXPECT_EQ(dsc::displayWidth("中"), 2u);
    EXPECT_EQ(dsc::displayWidth("a中b"), 4u);
}

// 末尾被截断的多字节序列不能越界读；每个凑不成序列的字节按 2 列兜底
TEST(DisplayWidth, TruncatedSequenceIsSafe) {
    std::string s = "a";
    s.push_back((char) 0xE4);
    s.push_back((char) 0xB8);
    EXPECT_EQ(dsc::displayWidth(s), 5u);
}

// ---- wrapDisplayText ----

TEST(WrapDisplayText, EmptyStaysEmpty) { EXPECT_EQ(dsc::wrapDisplayText("", 10, 0), ""); }

TEST(WrapDisplayText, ExactlyWidthNoWrap) {
    EXPECT_EQ(dsc::wrapDisplayText("abcdefghij", 10, 0), "abcdefghij");
}

TEST(WrapDisplayText, PrefersSpace) {
    EXPECT_EQ(dsc::wrapDisplayText("hello world", 8, 0), "hello\nworld");
}

TEST(WrapDisplayText, NoSpaceBreaksMidWord) {
    EXPECT_EQ(dsc::wrapDisplayText("abcdefghijk", 5, 0), "abcde\nfghij\nk");
}

TEST(WrapDisplayText, ContinuationIndent) {
    EXPECT_EQ(dsc::wrapDisplayText("aa bb cc", 5, 3), "aa bb\n   cc");
}

// 宽度比单个汉字还窄时不能无限循环，只能一行放一个码点
TEST(WrapDisplayText, WidthSmallerThanCjk) {
    EXPECT_EQ(dsc::wrapDisplayText("中文", 1, 0), "中\n文");
}

TEST(WrapDisplayText, CjkNeverSplitsCodepoint) {
    const std::string out = dsc::wrapDisplayText("中文中文中文", 5, 0);
    EXPECT_EQ(out, "中文\n中文\n中文");
    EXPECT_TRUE(isValidUtf8(out));
    for (const auto &line: splitLines(out))
        EXPECT_TRUE(isValidUtf8(line));
}

// 空格正好落在段首时不能产生空行
TEST(WrapDisplayText, SpaceAtSegmentStartNoEmptyLine) {
    const std::string out = dsc::wrapDisplayText(" abc", 1, 2);
    for (const auto &line: splitLines(out))
        EXPECT_FALSE(line.size() > 2 && line.substr(2).empty());
    EXPECT_TRUE(isValidUtf8(out));
}

// ---- formatHelp ----

TEST(FormatHelp, ShortLineUnchanged) { EXPECT_EQ(dsc::formatHelp("  --foo  bar\n", 100), "  --foo  bar\n"); }

TEST(FormatHelp, EmptyLinesPreserved) { EXPECT_EQ(dsc::formatHelp("a\n\nb\n", 100), "a\n\nb\n"); }

// 回归：前缀必须包含选项名和描述之间的两个空格，否则输出会变成
// "--load把原始..." 且续行缩进少 2 列
TEST(FormatHelp, KeepsDescriptionSeparator) {
    const std::string line = "  --load  " + std::string("把原始二进制文件加载到RAM物理地址中以便测试折行效果");
    const std::string out = dsc::formatHelp(line + "\n", 40);
    const auto lines = splitLines(out);

    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0].substr(0, 10), "  --load  ");
    EXPECT_EQ(lines[0].substr(10, 3), "把");
    // 续行的缩进必须等于描述列（10 显示列）
    for (std::size_t i = 1; i < lines.size(); i++)
        EXPECT_EQ(leadingSpaces(lines[i]), 10u) << "line " << i;
    for (const auto &line: lines)
        EXPECT_LE(dsc::displayWidth(line), 40u);
    EXPECT_TRUE(isValidUtf8(out));
}

TEST(FormatHelp, AsciiDescriptionWrapsAtSpaces) {
    const std::string line = "  --option  " + std::string("aaaa bbbb cccc dddd eeee ffff gggg");
    const auto lines = splitLines(dsc::formatHelp(line + "\n", 30));
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0].substr(0, 12), "  --option  ");
    for (std::size_t i = 1; i < lines.size(); i++)
        EXPECT_EQ(leadingSpaces(lines[i]), 12u);
    for (const auto &line: lines)
        EXPECT_LE(dsc::displayWidth(line), 30u);
}

// 没有双空格分界的行（usage、分组标题）按整行折，续行保持原缩进
TEST(FormatHelp, UsageLineWrappedWithOriginalIndent) {
    const std::string line = "  usage: dcs_soc circuit.json --elf boot.elf --disk disk.img --forever";
    const std::string out = dsc::formatHelp(line + "\n", 30);
    const auto lines = splitLines(out);
    ASSERT_GE(lines.size(), 2u);
    for (const auto &line: lines)
        EXPECT_LE(dsc::displayWidth(line), 30u);
    for (std::size_t i = 1; i < lines.size(); i++)
        EXPECT_EQ(leadingSpaces(lines[i]), 2u);
    EXPECT_TRUE(isValidUtf8(out));
}

TEST(FormatHelp, LongAsciiWithoutSeparator) {
    const std::string line(60, 'x');
    const auto lines = splitLines(dsc::formatHelp(line + "\n", 20));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], std::string(20, 'x'));
    EXPECT_EQ(lines[1], std::string(20, 'x'));
    EXPECT_EQ(lines[2], std::string(20, 'x'));
}

// 选项名比整行宽度还长时仍要终止；缩进可能超出目标宽度，但不能丢字符或死循环
TEST(FormatHelp, PrefixWiderThanWidthTerminates) {
    const std::string line = "  --verylongoption  " + std::string("描述文字继续折行");
    const std::string out = dsc::formatHelp(line + "\n", 10);
    EXPECT_NE(out.find("描述"), std::string::npos);
    EXPECT_TRUE(isValidUtf8(out));
    EXPECT_EQ(out.back(), '\n');
}

// 短行原样保留，混合长短行时只有长行被重排
TEST(FormatHelp, MixedLines) {
    const std::string input = "short\n" + std::string("  --x  ") + std::string(50, 'y') + "\n";
    const auto lines = splitLines(dsc::formatHelp(input, 20));
    EXPECT_EQ(lines[0], "short");
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines[1].substr(0, 7), "  --x  ");
}
