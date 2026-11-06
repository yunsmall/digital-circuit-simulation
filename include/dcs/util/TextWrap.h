// 按终端显示列折行 UTF-8 文本，以及重排 cxxopts 的 --help。
// cxxopts 自带的折行按字节数数，会在 UTF-8 序列中间下刀，中文帮助在换行处变乱码；
// 它的 CXXOPTS_USE_UNICODE 又按 UTF-16 码元数而不是显示列数算，中文仍然对不齐。
// 这里在输出层自己重排：ASCII 算 1 列，其它码点算 2 列，只在码点边界断开。
#pragma once
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace dsc {

inline std::size_t utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

inline std::size_t displayWidth(std::string_view s) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char) s[i];
        std::size_t len = utf8CharLen(c);
        if (i + len > s.size()) len = 1;
        width += (c < 0x80) ? 1 : 2;
        i += len;
    }
    return width;
}

// 把一段文字折成每行正文不超过 width 显示列；续行另加 indent 个空格，
// 所以续行的整行宽度是 indent + width。优先在空格处断，没有空格就按码点边界断
inline std::string wrapDisplayText(const std::string &text, std::size_t width, std::size_t indent) {
    std::string out;
    std::size_t seg = 0, i = 0, col = 0;
    std::size_t last_space = std::string::npos, last_space_col = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char) text[i];
        std::size_t len = utf8CharLen(c);
        if (i + len > text.size()) len = 1;
        const std::size_t w = (c < 0x80) ? 1 : 2;
        if (c == ' ') {
            last_space = i;
            last_space_col = col;
        }
        if (col + w > width && i > seg) {
            std::size_t cut = i, cut_col = col;
            if (last_space != std::string::npos && last_space > seg) {
                cut = last_space;
                cut_col = last_space_col;
            }
            out.append(text, seg, cut - seg);
            out.push_back('\n');
            out.append(indent, ' ');
            i = cut;
            if (i < text.size() && text[i] == ' ') ++i;
            seg = i;
            col = 0;
            last_space = std::string::npos;
            (void) cut_col;
            continue;
        }
        col += w;
        i += len;
    }
    out.append(text, seg, std::string::npos);
    return out;
}

// 重排 cxxopts 的 help：带"选项 + 两空格 + 描述"的行按描述列折行，
// 其余超宽行按整行折。输入必须是没被 cxxopts 折过的（宽度设得很大）
inline std::string formatHelp(const std::string &help, std::size_t width = 100) {
    std::istringstream in(help);
    std::string out, line;
    while (std::getline(in, line)) {
        if (displayWidth(line) <= width) {
            out += line;
            out.push_back('\n');
            continue;
        }
        // 第一个非行首空格后出现的"两个连续空格"是选项列与描述列的分界
        std::size_t first = line.find_first_not_of(' ');
        std::size_t desc = std::string::npos;
        for (std::size_t i = (first == std::string::npos ? 0 : first); i + 1 < line.size(); ++i) {
            if (line[i] == ' ' && line[i + 1] == ' ' && (i + 2 >= line.size() || line[i + 2] != ' ')) {
                desc = i;
                break;
            }
        }
        if (desc != std::string::npos && desc + 2 < line.size()) {
            std::size_t text_pos = line.find_first_not_of(' ', desc);
            if (text_pos != std::string::npos) {
                // 前缀取到描述起点：两个分隔空格要保留，否则描述会粘在选项名后面，
                // 续行缩进也会比描述列少 2 列
                const std::string prefix = line.substr(0, text_pos);
                const std::size_t prefix_width = displayWidth(prefix);
                out += prefix;
                out += wrapDisplayText(line.substr(text_pos), width > prefix_width ? width - prefix_width : 10,
                                       prefix_width);
                out.push_back('\n');
                continue;
            }
        }
        // 没有描述列（usage/分组标题等）：去掉行首缩进后整行折，续行补回同样的缩进。
        // 正文宽度要扣掉缩进，否则续行整行会变成 indent + width
        const std::size_t indent = (first == std::string::npos ? 0 : first);
        out.append(indent, ' ');
        out += wrapDisplayText(line.substr(indent), width > indent ? width - indent : 10, indent);
        out.push_back('\n');
    }
    return out;
}

} // namespace dsc
