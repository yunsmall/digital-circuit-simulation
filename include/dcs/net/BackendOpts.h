// 网络后端的私有选项
//
// 元件参数表在 JSON 里就是 "params": {"k": "v", ...} 的扁平结构，而后端专有的
// 配置（slirp 的 hostfwd、tap 的网卡名……）天然是多键的。为了不给每个后端往
// 元件通用参数表里加字段、也不让每个后端各写一套字符串解析，约定：
//
//   JSON 里写成嵌套对象，加载时按键路径拍平成 "backend_opts.hostfwd" 这样的扁平键
//   （见 CircuitSerializer 的 flattenParams），元件只保留自己前缀下的那些键。
//
// 例：
//   "params": { "backend": "slirp",
//               "backend_opts": { "hostfwd": "tcp:127.0.0.1:2223-10.0.2.15:23" } }
#pragma once
#include <map>
#include <string>
#include <unordered_map>

namespace dsc {

// 元件私有选项的前缀：JSON 里 backend_opts 对象的每个键会变成 backend_opts.<key>
inline constexpr const char *kBackendOptsPrefix = "backend_opts.";

// 从元件参数表里摘出自己前缀下的选项
inline std::map<std::string, std::string> takeBackendOpts(const std::unordered_map<std::string, std::string> &params) {
    std::map<std::string, std::string> opts;
    const std::string prefix = kBackendOptsPrefix;
    for (auto &[k, v]: params)
        if (k.compare(0, prefix.size(), prefix) == 0)
            opts[k.substr(prefix.size())] = v;
    return opts;
}

} // namespace dsc
