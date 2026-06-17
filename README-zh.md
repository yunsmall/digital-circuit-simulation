# 数字电路仿真

[English](README.md)

基于 JIT 编译的数字电路仿真框架。从元件构建电路，用 JSON 描述，通过 LLVM/Clang ORC JIT 或 TinyCC 编译为原生机器码高速仿真。

## 特性

- **50+ 内建元件**：门、触发器、寄存器、计数器、加减乘除、比较器、浮点、RAM、ROM、FIFO、桶形移位器、符号扩展、优先级编码器、多端口 RAM 等
- **JIT 编译**：电路 → C 代码 → 机器码，内存中完成
- **双后端**：LLVM/Clang ORC JIT（默认）或 TinyCC（轻量替代）
- **JSON 导入/导出**：完整电路描述，可往返
- **复合元件**：共享蓝图的子电路（定义一次半加器，实例化十次）
- **DLL 元件**：运行时加载外部 Verilog/Verilator/C 模块
- **Verilator 集成**：Verilog → C++ → DLL → 仿真（可选）
- **可配置位宽**：1~64 位 / 线网
- **组合环路检测**：三色 DFS + 报告
- **参数校验**：非法值在 JSON 加载时报错
- **gtest 测试**：17 个测试目标，覆盖全部元件类型

## 快速开始

### Windows（LLVM 后端）

```bash
cmake -B build \
  -DLLVM_ROOT=E:/path/to/clang+llvm-22.1.7-x86_64-pc-windows-msvc \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --parallel --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

### Windows（TCC 后端）

```bash
cmake -B build_tcc -DDCS_USE_TCC=ON \
  -DTCC_ROOT=E:/path/to/tinycc/win32 \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build_tcc --parallel --config RelWithDebInfo
ctest --test-dir build_tcc -C RelWithDebInfo --output-on-failure
```

`TCC_ROOT` 可省略——未指定时 CMake 自动搜索系统路径。

### Linux / WSL（TCC 后端）

```bash
# 安装 TCC 开发包（可选，否则需指定 TCC_ROOT）
sudo apt install libtcc-dev

cmake -B build_tcc -DDCS_USE_TCC=ON \
  -DDCS_BUILD_VERILOG=OFF \
  -DCMAKE_TOOLCHAIN_FILE=/home/user/.local/share/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build_tcc --parallel
ctest --test-dir build_tcc --output-on-failure
```

### Verilator 集成（可选）

需 MSYS2/UCRT64 的 Verilator：

```bash
cmake -B build \
  -Dverilator_ROOT=E:/msys64/ucrt64/local \
  ...（其它参数）
```

不需要时关掉：`-DDCS_BUILD_VERILOG=OFF`。

### CLI 用法

```bash
# 仿真
./build/RelWithDebInfo/dcs circuit.json -t 20 -d 500 -m

# JSON → C 代码
./build/RelWithDebInfo/dcs --json-to-c circuit.json

# 列出所有元件类型及参数
./build/RelWithDebInfo/dcs --list-types

# 查看 DLL 元件信息
./build/RelWithDebInfo/dcs --dll-info adder8_dll.dll
```

## 作为子目录集成

```cmake
set(DCS_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_VERILOG OFF CACHE BOOL "" FORCE)
add_subdirectory(digital_circuit_simulation)

target_link_libraries(my_app PRIVATE dcs_lib)
```

## 元件类型

### 组合逻辑

| 类型 | 说明 | 参数 |
|------|------|------|
| `and`, `or`, `nand`, `nor`, `xor`, `xnor` | 多输入门 | `inputs`（2~8）, `bit_width`（1~64） |
| `not` | 按位取反 | `bit_width` |
| `buf` | 缓冲器 | `bit_width` |
| `tsbuf` | 三态缓冲（oe 控制） | `bit_width` |
| `mux` | 多路选择器 (2^N:1) | `n_selects`（1~4）, `bit_width` |
| `adder` | 全加器: {cout, sum} = a + b + cin | `bit_width` |
| `sub` | 减法器: {bout, diff} = a - b - bin | `bit_width` |
| `mul` | 乘法器: {prod_hi, prod_lo} = a × b | `bit_width`（1~64）, `signed`（0/1） |
| `div` | 除法器: {quot, rem} = a / b | `bit_width`, `signed` |
| `comparator` | 比较器 | `bit_width`, `op`（eq/ne/lt/gt/le/ge） |
| `decoder` | N 位 → 2^N one-hot | `n_selects`（1~8） |
| `encoder` | 2^N one-hot → N 位 | `n_selects`（1~8） |
| `prienc` | 优先级编码器 | `num_inputs`（2~64） |
| `signext` | 符号扩展 | `src_width`（1~63）, `dst_width`（2~64） |
| `barrel` | 桶形移位器 | `bit_width`（2~64） |
| `constant` | 固定常量 | `bit_width`, `value` |
| `switch` | 条件通路 | `bit_width` |
| `merge` | N×1bit → N 位总线 | `num_bits`（2~64） |
| `split` | N 位总线 → N×1bit | `num_bits`（2~64） |

### 浮点（IEEE 754）

| 类型 | 说明 | 参数 |
|------|------|------|
| `fadd`, `fsub`, `fmul`, `fdiv` | 浮点四则运算 | `precision`（32/64） |
| `fcmp` | 浮点比较 | `precision`, `op`（eq/ne/lt/gt/le/ge） |
| `fconst` | 浮点常量 | `precision`, `value`（如 "3.14"） |

### 时序逻辑

| 类型 | 说明 | 参数 |
|------|------|------|
| `dff` | D 触发器（上升沿） | `bit_width`，可选 `has_en`/`has_rst`/`has_preset` |
| `tff` | T 触发器（翻转） | `bit_width` |
| `jkff` | JK 触发器 | `bit_width` |
| `register` | 多位寄存器 | `bit_width`，可选 `has_en`/`has_rst` |
| `latch` | D 锁存器（电平敏感） | `bit_width` |
| `counter` | 加减计数器 | `bit_width`，可选 `has_load`/`has_en`/`has_updown`/`has_clr` |
| `shift` | 移位寄存器 | `length`，可选 `has_dir`/`has_clr` |
| `clkgen` | 时钟发生器 | `high_ticks`, `low_ticks` |
| `delay` | N 级延时线 | `bit_width`, `depth`（1~256）, `use_clock`（0/1） |

### 存储器

| 类型 | 说明 | 参数 |
|------|------|------|
| `memory` | 带读写延迟的 RAM | `addr_width`, `data_width`, `read_latency`, `write_latency` |
| `rom` | ROM（hex / raw 字节 / 文件 mmap） | `addr_width`, `data_width`, `source_type`（hex/file）, `filepath`, `use_mmap` |
| `mpram` | 多端口 RAM（1~4 读 + 1~2 写，含 rd_valid 流控） | `addr_width`, `data_width`, `num_read_ports`, `num_write_ports`, `read_latency` |
| `fifo` | 同步 FIFO | `data_width`, `depth`（2 的幂）, `has_rst` |

### 工具

| 类型 | 说明 | 参数 |
|------|------|------|
| `print` | 打印整数线网值 | `bit_width` |
| `fprint` | 打印浮点线网值 | `precision`（32/64） |
| `dll` | 外部 DLL 元件 | `path` |
| `composite` | 可复用子电路 | `definition`（蓝图名） |

## JSON 电路格式

```json
{
  "definitions": {
    "half_adder": {
      "nets": ["a", "b", "sum", "carry"],
      "components": [
        { "type": "xor", "name": "xor1", "params": {"inputs":"2","bit_width":"1"},
          "connections": {"in0":"a","in1":"b","out":"sum"} },
        { "type": "and", "name": "and1", "params": {"inputs":"2","bit_width":"1"},
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

## 技术架构

```
JSON / C++ API
      │
      ▼
  Circuit（线网 + 元件容器）
      │
      ├─ expandComposites() — 展开子电路
      ├─ check()            — 组合环路检测（三色 DFS）
      │
      ▼
  generateC() — 各元件生成 C 代码片段
      │
      ▼
  JIT 编译（LLVM Clang ORC / TinyCC）
      │
      ▼
  可执行函数: circuit_init / circuit_tick / set_wire / get_wire
```

- C++23, CMake 4.2
- LLVM/Clang 22.1.7: Clang API 内存编译 C → LLVM IR → ORC JIT
- TinyCC: libtcc 内存编译 C → 直接执行
- gtest + vcpkg 管理测试
- `std::format` + 原始字符串字面量生成 C 代码
- 线网最大位宽 64 位（内部 16 字节/槽）

## 添加新元件

1. **头文件** `include/dcs/components/<Name>.h` — 继承 `CombinationalComponent`（组合）或 `SequentialComponent`（时序）
2. **实现** `src/components/<Name>.cpp` — 实现 `genFuncDef_comb()` / `genFuncDef_seq()` + `clone()`
3. **注册** `src/ComponentFactory.cpp` — `InitAllComponents()` 中 `f.registerType(...)`，加 `#include`
4. **测试** `tests/test_<name>/` — 按功能拆 `.cpp`，参考已有测试结构
5. `cmake -B build ...` 重新配置（GLOB 拾取新文件）

### 基类速查

| 基类 | hasCombinational() | hasSequential() | 需实现 |
|------|---------------------|-----------------|--------|
| `CombinationalComponent` | true | false | `genFuncDef_comb()` |
| `SequentialComponent` | false | true | `genFuncDef_seq()`, `genStateDecl()`, `genInitCode()` |

### C 代码生成辅助函数

- `gen_read_wire(net_id, pin_w, net_w, var)` — 读线网值（变量自动 `= 0`）
- `gen_write_wire(comp_id, out_idx, val, pin_w)` — 写线网值
- `c_int_type(width)` — 位宽 → C 类型（uint8/16/32/64_t）
- `gen_mask(width)` — 位宽 → 掩码字面量
- `byte_count(width)` — 位宽 → 字节数

## 许可证

MIT
