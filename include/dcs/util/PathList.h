// PATH 风格的路径列表拆分，CLI 用于解析 DCS_LIB_PATH 等环境变量
#pragma once
#include <string>
#include <vector>

namespace dsc {

// Windows 用 ';' 分隔，其它平台用 ':'（和 PATH 一致）。空段（末尾多写一个分隔符）
// 会被忽略，不做通配符处理
inline std::vector<std::string> splitPathList(const std::string &s) {
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(sep, pos);
        if (next == std::string::npos)
            next = s.size();
        if (next > pos)
            out.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

} // namespace dsc
