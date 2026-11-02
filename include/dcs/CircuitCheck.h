// 电路/蓝图文件的校验工具：只做加载、展开和静态检查，不编译 JIT。
//
// 给 CLI 的 --check / --list-definitions 用，也可以被集成方直接调用。
#pragma once
#include <string>
#include <vector>

namespace dsc {

class Context;

struct CheckReport {
    int nets = 0; // 展开复合元件之后的线网数
    int components = 0; // 展开之后的元件数（蓝图校验时是所有定义的合计）
    std::vector<std::string> warnings; // 拓扑告警（悬空从端口、死路等）
    std::vector<std::string> definitions; // 上下文里的蓝图："名字（来源）"
};

// 校验顶层电路 JSON（$meta.kind 省略或为 circuit）。ctx 为空时用进程默认上下文
bool checkCircuitFile(const std::string &path, Context *ctx, CheckReport &report, std::string &error);

// 校验蓝图 JSON：把文件里的每个定义各实例化一次再检查，蓝图之间可以互相引用
bool checkBlueprintFile(const std::string &path, Context *ctx, CheckReport &report, std::string &error);

// 读文件的 $meta.kind（没有 $meta 时按 "circuit"）
bool readJsonKind(const std::string &path, std::string &kind, std::string &error);

// 只把文件里的 definitions 导入上下文（电路或蓝图都行），不检查元件
bool loadDefinitionsFromFile(const std::string &path, Context *ctx, std::string &error);

} // namespace dsc
