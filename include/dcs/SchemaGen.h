// 从已注册元件的元数据生成 JSON Schema，给编辑器提示用（运行期不做校验）
#pragma once
#include <string>

namespace dsc {

// 生成"单个元件对象"的 schema：type 是已注册类型名的枚举，params 按类型给出
// 参数名、默认值和可选值。格式版本变化时同步改这里
std::string componentSchemaJson();

} // namespace dsc
