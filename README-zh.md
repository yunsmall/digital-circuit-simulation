# 数字电路仿真

基于 JIT 编译的数字电路仿真框架。从元件（门电路、触发器、运算器、存储器、浮点等）构建电路，用 JSON 描述，通过 LLVM/Clang ORC JIT 或 TinyCC 高速仿真。

## 特性

- **40+ 内置元件**：门电路、触发器、寄存器、计数器、加法器、乘法器、除法器、比较器、float32/float64 浮点运算、RAM、ROM、时钟发生器、打印监控等
- **JIT 编译**：电路 → C 代码 → 机器码，全部在内存中完成
- **双后端**：LLVM/Clang ORC JIT（默认）或 TinyCC（轻量替代）
- **JSON 导入导出**：完整电路定义，可往返
- **复合元件**：共享蓝图的子电路定义（如定义一次半加器，实例化十次）
- **DLL 元件**：运行时加载外部 Verilog/Verilator/C 模块
- **可配置位宽**：每条线网 1–64 位
- **组合环路检测**：三色 DFS，报告参与环路的线网
- **gtest 测试**：9 个测试文件覆盖全部元件类型

## 快速开始

```bash
# 配置（需要 vcpkg）
cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --parallel --config RelWithDebInfo

# 运行测试
ctest --test-dir build -C RelWithDebInfo --output-on-failure

# 仿真电路
./build/RelWithDebInfo/digital_circuit_simulation circuit.json -t 20 -d 500

# JSON 转 C 代码
./build/RelWithDebInfo/dcs_tool --json-to-c circuit.json > circuit.c

# 列出所有元件类型
./build/RelWithDebInfo/dcs_tool --list-types
```

## 依赖

- C++23 编译器（MSVC 2022、Clang 18+、GCC 14+）
- CMake 3.28+
- [vcpkg](https://github.com/microsoft/vcpkg) 包：`nlohmann-json`、`cxxopts`、`gtest`（可选）
- LLVM/Clang 22.1.7（默认后端）或 TinyCC（替代）

## 仿真程序参数

```
digital_circuit_simulation circuit.json [选项]

  -t, --ticks N    仿真 tick 数（默认 10）
  -d, --delay MS   每 tick 现实毫秒数（默认 1000）
  -m, --monitor    逐 tick 打印线网值
  -h, --help       显示帮助
```

## CLI 工具

```
dcs_tool --list-types              列出所有已注册的元件类型及参数
dcs_tool --json-to-c circuit.json  将 JSON 电路编译为 C 代码
dcs_tool --help                    显示帮助
```

## 元件类型

### 组合逻辑

| 名称 | 功能 | 参数 |
|------|------|------|
| `and`、`or`、`nand`、`nor`、`xor`、`xnor` | 多输入门 | `inputs`（2–8）、`bit_width`（1–64） |
| `not` | 按位取反 | `bit_width` |
| `buf` | 缓冲器 | `bit_width` |
| `tsbuf` | 三态缓冲器 | `bit_width` |
| `mux` | 多路选择器（2^N:1） | `n_selects`（1–4）、`bit_width` |
| `adder` | 全加器：{cout, sum} = a + b + cin | `bit_width` |
| `sub` | 减法器：{bout, diff} = a - b - bin | `bit_width` |
| `mul` | 乘-法器（有/无符号）：prod = a × b | `bit_width`（≤32）、`signed` |
| `div` | 除法器（有/无符号）：{quot, rem} = a / b | `bit_width`（≤64）、`signed` |
| `comparator` | 比较器 | `bit_width`、`op`（eq/ne/lt/gt/le/ge）、`signed` |
| `decoder` | 译码器：N 位 → 2^N 位 one-hot | `n_selects`（1–8） |
| `encoder` | 编码器：2^N 位 one-hot → N 位 | `n_selects`（1–8） |
| `constant` | 整型常量 | `bit_width`、`value` |
| `switch` | 条件导通 | `bit_width` |
| `merge` | N×1 位 → N 位总线 | `num_bits`（2–64） |
| `split` | N 位总线 → N×1 位 | `num_bits`（2–64） |

### 浮点（IEEE 754）

| 名称 | 功能 | 参数 |
|------|------|------|
| `fadd`、`fsub`、`fmul`、`fdiv` | float32/64 算术运算 | `precision`（32 或 64） |
| `fcmp` | 浮点比较 | `precision`、`op`（eq/ne/lt/gt/le/ge） |
| `fconst` | 浮点常量 | `precision`、`value`（如 "3.14"） |

### 时序逻辑

| 名称 | 功能 | 参数 |
|------|------|------|
| `dff` | D 触发器（上升沿） | `bit_width`，可选 `has_en`/`has_rst`/`has_preset` |
| `tff` | T 触发器（翻转） | `bit_width` |
| `jkff` | JK 触发器 | `bit_width` |
| `register` | 多位寄存器 | `bit_width`，可选 `has_en`/`has_rst` |
| `latch` | D 锁存器（电平敏感） | `bit_width` |
| `counter` | 计数器 | `bit_width`，可选 load/enable/direction/clear |
| `shift` | 移位寄存器 | `length`，可选 direction/clear |
| `clkgen` | 时钟发生器（可配占空比） | `high_ticks`、`low_ticks` |

### 存储器

| 名称 | 功能 | 参数 |
|------|------|------|
| `memory` | RAM（可配读写延迟） | `addr_width`、`data_width`、`read_latency`、`write_latency` |
| `rom` | ROM（hex 初始化） | `addr_width`、`data_width`、`initial_data`（hex）、`read_latency` |

### 工具

| 名称 | 功能 | 参数 |
|------|------|------|
| `print` | 时钟沿打印输入值 | `bit_width` |
| `dll` | 外部 DLL 元件 | `path` |
| `composite` | 可复用子电路 | `definition`（蓝图名） |

## JSON 电路格式

```json
{
  "definitions": {
    "half_adder": {
      "nets": ["a", "b", "sum", "carry"],
      "components": [
        { "type": "xor", "name": "xor", "params": {"inputs":"2","bit_width":"1"},
          "connections": {"in0":"a","in1":"b","out":"sum"} },
        { "type": "and", "name": "and", "params": {"inputs":"2","bit_width":"1"},
          "connections": {"in0":"a","in1":"b","out":"carry"} }
      ],
      "expose": { "inputs": {"a":"a","b":"b"}, "outputs": {"sum":"sum","carry":"carry"} }
    }
  },
  "nets": ["a0", "b0", "s0", "c0"],
  "components": [
    { "type": "composite", "name": "ha", "params": {"definition":"half_adder"},
      "connections": {"a":"a0","b":"b0","sum":"s0","carry":"c0"} }
  ]
}
```

用 `dcs_tool --json-to-c` 转 C 代码，或 `digital_circuit_simulation circuit.json` 直接仿真。

## 架构

```
JSON / C++ API
      │
      ▼
  Circuit（容器：线网 + 元件）
      │
      ├─ expandComposites()  — 展开子电路
      ├─ check()             — 组合环路检测（三色 DFS）
      │
      ▼
  generateC()  — 各元件输出 C 代码片段
      │
      ▼
  JIT 编译（LLVM Clang ORC / TinyCC）
      │
      ▼
  可执行函数：circuit_init / circuit_tick / set_wire / get_wire
```

## 添加新元件

1. 声明类 `include/dcs/components/<Name>.h`
2. 实现 `genFuncDef()` + `clone()` 于 `src/components/<Name>.cpp`
3. 注册到 `src/ComponentFactory.cpp`（`InitAllComponents` 构造函数中）
4. 添加测试 `tests/test_<name>.cpp`
5. 重新 `cmake -B build ...` 以让 CMake GLOB 发现新文件

### 基类速查

| 基类 | 用途 | C 代码生成要点 |
|------|------|---------------|
| `CombinationalComponent` | 组合逻辑 | `genFuncDef()` 读取输入线网→计算→写入输出线网 |
| `SequentialComponent` | 时序逻辑 | 需额外实现 `genStructDef()`、`genStateDecl()`、`genInitCode()` |

### C 代码辅助函数

- `gen_read_wire(net_id, pin_w, net_w, var)` — 从线网读取值
- `gen_write_wire(net_id, val, pin_w)` — 写入值到线网
- `c_int_type(width)` — 位宽→C 类型（uint8_t/uint16_t/uint32_t/uint64_t）
- `gen_mask(width)` — 位宽→掩码字面量
- `byte_count(width)` — 位宽→字节数
- `setParam(key, val)` — 记录构造参数（供 JSON 导出）

## 项目结构

```
digital_circuit_simulation/
├── include/dcs/
│   ├── Circuit.h、Component.h、Net.h、Pin.h
│   ├── CircuitLibrary.h、ComponentFactory.h
│   └── components/          # 40+ 元件头文件
├── src/
│   ├── Circuit.cpp、ComponentFactory.cpp、CircuitLibrary.cpp
│   └── components/          # 元件实现
├── tools/
│   └── dcs_tool.cpp         # CLI：列类型、JSON→C
├── tests/                   # gtest 测试
├── shim/                    # JIT 编译用的 C 头文件
├── main.cpp                 # 仿真运行器
└── CMakeLists.txt
```

## 许可证

MIT
