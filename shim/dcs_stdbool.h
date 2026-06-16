// shim: 供 JIT 编译生成的 C 代码使用（C11 标准风格）
#pragma once

#ifndef __cplusplus
#define bool _Bool
#define true 1
#define false 0
#endif
