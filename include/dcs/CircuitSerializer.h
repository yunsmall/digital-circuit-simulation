// CircuitSerializer — 电路与 JSON 之间的转换
//
// 只做"文本 <-> 网表"的翻译，不参与编译与仿真。
#pragma once
#include <memory>
#include <string>

namespace dsc {

class Circuit;

// base_dir 用于解析复合元件定义里的相对路径；失败时写 error 并返回 nullptr
std::unique_ptr<Circuit> loadCircuitFromJson(const std::string &json_str, std::string &error,
                                             const std::string &base_dir = "");

} // namespace dsc
