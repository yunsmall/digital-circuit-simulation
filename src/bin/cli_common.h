#pragma once
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <dcs/Circuit.h>
#include <dcs/Context.h>
#include <dcs/util/PathList.h>

// 两个 CLI（dcs / dcs_soc）共用的加载与参数解析。放在 src/bin 而不是 dcs 库：
// 这些是命令行入口的约定（输出格式、查找路径优先级），不是仿真库的接口
namespace dcs_cli {

// --lib-path 在前，环境变量 DCS_LIB_PATH（PATH 风格）在后。
// 必须在任何 fromJson 之前调用，电路 JSON 的 definitions 会用到查找路径
inline void configureLibrary(dsc::Context &ctx, const std::vector<std::string> &lib_paths) {
    for (auto &p: lib_paths)
        ctx.library().addSearchPath(p);
    if (const char *env = std::getenv("DCS_LIB_PATH"))
        for (auto &p: dsc::splitPathList(env))
            ctx.library().addSearchPath(p);
}

// 读 JSON → 建电路。失败时打印原因并返回空
inline std::unique_ptr<dsc::Circuit> loadCircuit(const std::string &path, dsc::Context &ctx) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << std::format("错误: 无法打开 '{}'\n", path);
        return nullptr;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string error;
    auto circuit =
            dsc::Circuit::fromJson(oss.str(), error, std::filesystem::path(path).parent_path().string(), &ctx);
    if (!circuit) {
        std::cerr << std::format("JSON 错误: {}\n", error);
        return nullptr;
    }
    std::cout << std::format("电路: {} 线网, {} 元件\n", circuit->netCount(), circuit->componentCount());
    return circuit;
}

// --dump-mem 的 addr:len（十六进制）
inline bool parseAddrLen(const std::string &spec, size_t &addr, size_t &len, std::string &error) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos) {
        error = "--dump-mem 格式应为 addr:len（十六进制）";
        return false;
    }
    addr = std::stoull(spec.substr(0, colon), nullptr, 0);
    len = std::stoull(spec.substr(colon + 1), nullptr, 0);
    return true;
}

template <class T>
T *findFirstComponent(dsc::Circuit &c) {
    for (auto &comp: c.components())
        if (auto *p = dynamic_cast<T *>(comp.get()))
            return p;
    return nullptr;
}

} // namespace dcs_cli
