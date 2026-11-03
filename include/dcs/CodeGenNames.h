#pragma once
#include <format>
#include <string>

namespace dsc::cgen {

// 生成 C 代码里的全局命名只在这里拼。CodeGen 负责声明、元件负责使用，
// 同一条规则散在两边时，改前缀/改布局漏一处就是运行期读到别的静态变量
inline std::string wire(int net_id) {
    return std::format("_w[{}]", net_id);
}

inline std::string cOut(int comp_id, int out_idx) {
    return std::format("_c_out_{}_{}", comp_id, out_idx);
}

inline std::string cOe(int comp_id, int out_idx) {
    return std::format("_c_oe_{}_{}", comp_id, out_idx);
}

} // namespace dsc::cgen
