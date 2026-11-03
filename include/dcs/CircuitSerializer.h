// CircuitSerializer — 电路与 JSON 之间的转换
//
// 只做"文本 <-> 网表"的翻译，不参与编译与仿真。
#pragma once
#include <memory>
#include <string>

namespace dsc {

class Circuit;
class Context;

// JSON 数据格式版本，写在 $meta.format 里。新增可选字段不用升版本，
// 只有改变已有字段含义时才升；加载时拒绝比自己支持的新版本
inline constexpr int kJsonFormatVersion = 1;

// base_dir 用于解析复合元件定义里的相对路径；context 为空时用进程默认上下文。
// 失败时写 error 并返回 nullptr
std::unique_ptr<Circuit> loadCircuitFromJson(const std::string &json_str, std::string &error,
                                             const std::string &base_dir = "",
                                             Context *context = nullptr);

// 导出为电路 JSON（含 $meta 和当前上下文的 definitions）。
// 读写两侧放在同一文件里，字段增删时能直接对照
std::string exportCircuitJson(const Circuit &circuit);

} // namespace dsc
