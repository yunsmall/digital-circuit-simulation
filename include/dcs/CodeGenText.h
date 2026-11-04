// 生成 C 文本的净化工具
//
// 元件名、蓝图名等来自 JSON，属于用户可控输入。直接拼进生成的 C 源码会形成注入。
// 这里只做一件简单的事：只放行字母、数字、空格、下划线、点、横线、冒号和 UTF-8
// 高位字节，其余字符（换行、分号、括号、引号、斜杠、反斜杠、问号、大括号……）
// 一律换成下划线。这样注释、字符串字面量里都不会出现能改变 C 语法的字符
#pragma once
#include <string>
#include <string_view>

namespace dsc::cgen {

inline std::string commentText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c: text) {
        const bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          c == ' ' || c == '_' || c == '.' || c == '-' || c == ':' || c >= 0x80;
        out.push_back(keep ? (char) c : '_');
    }
    return out;
}

} // namespace dsc::cgen
