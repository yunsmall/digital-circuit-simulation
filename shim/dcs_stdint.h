// shim: 供 JIT 编译生成的 C 代码使用，标准 C 兼容，不依赖编译器扩展
// 用 include guard 而不是 #pragma once：这份内容会被拼进同一个生成文件，
// 多个 #pragma once 落在同一个翻译单元里没有意义，宏保护才是正确做法
#ifndef DCS_SHIM_STDINT_H
#define DCS_SHIM_STDINT_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;

// 生成的代码把这些类型当作精确宽度的寄存器值来回搬运：宽度不符会静默丢数据，
// 而且是仿真跑出错误结果之后才看得出来。用数组大小做编译期断言而不是
// _Static_assert，是因为 TinyCC 后端也要能编译这份 shim
typedef char dcs_assert_uint8[(sizeof(uint8_t) == 1) ? 1 : -1];
typedef char dcs_assert_uint16[(sizeof(uint16_t) == 2) ? 1 : -1];
typedef char dcs_assert_uint32[(sizeof(uint32_t) == 4) ? 1 : -1];
typedef char dcs_assert_uint64[(sizeof(uint64_t) == 8) ? 1 : -1];
typedef char dcs_assert_int8[(sizeof(int8_t) == 1) ? 1 : -1];
typedef char dcs_assert_int16[(sizeof(int16_t) == 2) ? 1 : -1];
typedef char dcs_assert_int32[(sizeof(int32_t) == 4) ? 1 : -1];
typedef char dcs_assert_int64[(sizeof(int64_t) == 8) ? 1 : -1];

#endif // DCS_SHIM_STDINT_H
