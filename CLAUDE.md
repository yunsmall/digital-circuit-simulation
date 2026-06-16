# 数字电路仿真 — 项目说明

## 编译与运行

```bash
# 配置（不指定生成器，cmake 默认使用 MSVC）
cmake -B build

# 编译
cmake --build build --parallel

# 运行
./build/RelWithDebInfo/digital_circuit_simulation.exe
```

## 测试

```bash
# 配置（需 vcpkg toolchain）
cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

# 编译测试（Debug CRT 与 vcpkg gtest 不匹配，用 RelWithDebInfo）
cmake --build build --parallel --config RelWithDebInfo

# 运行测试
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

LLVM/Clang 位于 `E:/YeSmallWorkstation3/clang+llvm-22.1.7-x86_64-pc-windows-msvc`，可通过环境变量 `LLVM_ROOT` 覆盖。

## TCC 后端（可选）

TinyCC 位于 `E:/YeSmallWorkstation3/software_build/tinycc/win32`，可通过环境变量 `TCC_ROOT` 覆盖。

```bash
# 配置
cmake -B build_tcc -DDCS_USE_TCC=ON -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build_tcc --parallel --config RelWithDebInfo

# 测试
ctest --test-dir build_tcc -C RelWithDebInfo --output-on-failure
```

## 技术栈

- C++23, CMake 4.2
- LLVM/Clang 22.1.7：Clang API 内存编译 C → LLVM IR，ORC JIT 执行
- TinyCC：libtcc 内存编译 C → 直接执行（轻量级替代后端）
- gtest 测试框架（vcpkg）
- std::format + 原始字符串字面量生成 C 代码
- 线网最大位宽 64 位

## 添加新元件

1. **头文件** `include/dcs/components/<Name>.h` — 类声明，继承 `CombinationalComponent` 或 `SequentialComponent`
2. **实现** `src/components/<Name>.cpp` — 实现 `genFuncDef()`（C 代码生成）和 `clone()`
3. **工厂注册** `src/ComponentFactory.cpp` — 在 `InitAllComponents()` 构造函数中添加 `f.registerType(...)`，同时加入对应 `#include`
4. **测试** `tests/test_<name>.cpp` — gtest 用例
5. CMake 的 `file(GLOB DCS_SOURCES ...)` 会自动拾取新文件，**但新增后需重新执行 `cmake -B build ...` 配置**

### 元件基类速查

| 基类 | 用途 | isSequential() | genStructDef() |
|------|------|----------------|----------------|
| `CombinationalComponent` | 组合逻辑（门、运算器） | false | 返回 "" |
| `SequentialComponent` | 时序逻辑（触发器、寄存器）| true | 返回结构体定义 |

### 构造函数中必须做的事

- 调用 `setParam("key", val)` 记录构造参数（供 JSON 导出）
- 调用 `addInput(name, bit_width)` / `addOutput(name, bit_width)` 创建引脚

### C 代码生成辅助函数（定义于 Component.h）

- `gen_read_wire(net_id, pin_w, net_w, var_name)` — 从线网读取值的 C 语句
- `gen_write_wire(net_id, val_expr, pin_w)` — 写入线网的 C 语句
- `c_int_type(width)` — 位宽→C 类型名（uint8_t/uint16_t/uint32_t/uint64_t）
- `gen_mask(width)` — 位宽→掩码字面量（如 0xFFULL）
- `byte_count(width)` — 位宽→字节数
