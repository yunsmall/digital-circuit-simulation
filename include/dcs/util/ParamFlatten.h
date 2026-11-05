#pragma once
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace dsc {

// 参数值除了标量，还可以是嵌套对象（后端私有选项之类）。对象按键路径拍平成
// "backend_opts.hostfwd" 这样的扁平键，参数表因此保持 string→string 不变，
// 元件按前缀取自己那一段；数组和其它值仍然 dump 成字符串。
// 顶层电路装载和蓝图导入必须用同一份规则，否则同一个 JSON 在两边解释不一致
// 递归深度设上限：嵌套 params 来自 JSON，深度上万时递归拍平会把栈打爆
inline void flattenParams(const nlohmann::json &j, const std::string &prefix,
                          std::unordered_map<std::string, std::string> &out, int depth = 0) {
    if (depth > 64)
        throw std::invalid_argument("参数嵌套超过 64 层，JSON 结构可能写错了");
    for (auto &[k, v]: j.items()) {
        const std::string key = prefix.empty() ? k : prefix + "." + k;
        if (v.is_object())
            flattenParams(v, key, out, depth + 1);
        else
            out[key] = v.is_string() ? v.get<std::string>() : v.dump();
    }
}

} // namespace dsc
