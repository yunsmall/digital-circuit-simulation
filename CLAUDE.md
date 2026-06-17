# 数字电路仿真 — 项目说明

## 编译

项目不包含硬编码的外部库路径，需通过 `-D` 指定 LLVM/Clang 安装位置。

```bash
# 基本配置（LLVM/Clang 后端，默认）
cmake -B build \
  -DLLVM_ROOT=E:/YeSmallWorkstation3/clang+llvm-22.1.7-x86_64-pc-windows-msvc \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --parallel --config RelWithDebInfo

# 运行 CLI
./build/RelWithDebInfo/dcs.exe --help
```

`LLVM_ROOT` 指向 LLVM/Clang 的安装前缀（含 `bin/`、`lib/`、`include/` 的那个目录）。

## TCC 后端（可选）

`TCC_ROOT` 指向 TinyCC 的可执行文件所在目录（如 `win32/`，内含 `tcc.exe`、`libtcc.lib`、`libtcc.dll`）。

```bash
cmake -B build_tcc \
  -DDCS_USE_TCC=ON \
  -DTCC_ROOT=E:/YeSmallWorkstation3/software_build/tinycc/win32 \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build_tcc --parallel --config RelWithDebInfo
```

## Verilator 集成（可选）

需 MSYS2/UCRT64 环境的 Verilator。`verilator_ROOT` 指向安装前缀。

```bash
cmake -B build \
  -Dverilator_ROOT=E:/msys64/ucrt64/local \
  ...（其它参数同上）
```

不需要时关掉：

```bash
cmake -B build -DDCS_BUILD_VERILOG=OFF ...
```

## 测试

```bash
# 配置（需 vcpkg toolchain + LLVM）
cmake -B build \
  -DLLVM_ROOT=E:/YeSmallWorkstation3/clang+llvm-22.1.7-x86_64-pc-windows-msvc \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --parallel --config RelWithDebInfo

# 运行测试
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

## 作为子目录集成到其他 CMake 项目

```cmake
set(LLVM_ROOT "..." CACHE PATH "")
set(DCS_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_VERILOG OFF CACHE BOOL "" FORCE)
add_subdirectory(digital_circuit_simulation)

target_link_libraries(my_app PRIVATE dcs_lib)
```

## 技术栈

- C++23, CMake 4.2
- LLVM/Clang 22.1.7：Clang API 内存编译 C → LLVM IR，ORC JIT 执行
- TinyCC：libtcc 内存编译 C → 直接执行（轻量级替代后端）
- gtest 测试框架（vcpkg）
- std::format + 原始字符串字面量生成 C 代码
- 线网最大位宽 64 位

## 版本号与发布

`main.cpp` 中 `--version` 返回的版本号硬编码为字符串。发布新 tag 时需同步更新：

```cpp
// main.cpp — 搜索 "dcs 0.0.0" 替换为新版本号
std::cout << "dcs X.Y.Z\n";
```

同时更新 CMakeLists.txt 顶部的 `project()` 版本：

```cmake
project(digital_circuit_simulation VERSION X.Y.Z)
```

两个地方必须一致，否则 `--version` 和 CMake 导出的版本对不上。

## 添加新元件

1. **头文件** `include/dcs/components/<Name>.h` — 类声明，继承 `CombinationalComponent` 或 `SequentialComponent`
2. **实现** `src/components/<Name>.cpp` — 实现 `genFuncDef_comb()`（组合逻辑 C 代码）和/或 `genFuncDef_seq()`（时序逻辑 C 代码），以及 `clone()`
3. **工厂注册** `src/ComponentFactory.cpp` — 在 `InitAllComponents()` 构造函数中添加 `f.registerType(...)`，同时加入对应 `#include`
4. **测试** — 参考已有结构（见下方）
5. CMake 的 `file(GLOB DCS_SOURCES ...)` 会自动拾取新文件，**但新增后需重新执行 `cmake -B build ...` 配置**，`add_dcs_test` 同理需重新配置

### 测试规范

每个元件的测试放在独立目录：`tests/test_<name>/`，目录内按功能拆分 `.cpp` 文件（如 `basic.cpp`、`edge_cases.cpp`）。在 CMakeLists.txt 中通过 `add_dcs_test(test_<name> tests/test_<name>/*.cpp)` 注册。

测试用例结构参考：

- **组合逻辑**（如 `test_barrel/barrel.cpp`）：构建电路 → `compile()` → `setWire()` 设输入 → `tick()` → `getWire()` 验输出
- **时序逻辑**（如 `test_sequential/dff.cpp`）：额外验证 `init()`/`reset()` 清零行为、时钟边沿行为
- **位宽遍历**（如 `test_bitwidths/`）：用 `common.h` 中的参数化辅助，覆盖 1~64 全位宽

### 元件基类速查

| 基类 | 用途 | hasCombinational() | hasSequential() |
|------|------|---------------------|-----------------|
| `CombinationalComponent` | 组合逻辑（门、运算器） | true | false |
| `SequentialComponent` | 时序逻辑（触发器、寄存器）| false | true |

### 构造函数中必须做的事

- 调用 `setParam("key", val)` 记录构造参数（供 JSON 导出）
- 调用 `addInput(name, bit_width)` / `addOutput(name, bit_width)` 创建引脚
- 时序输出的引脚用 `addOutput(name, bw, false, true)`（`is_sequential=true`）

### C 代码生成辅助函数（定义于 Component.h）

- `gen_read_wire(net_id, pin_w, net_w, var_name)` — 从线网读取值的 C 语句（变量自动 `= 0` 初始化）
- `gen_write_wire(comp_id, out_idx, val_expr, pin_w)` — 写入线网的 C 语句
- `c_int_type(width)` — 位宽→C 类型名（uint8_t/uint16_t/uint32_t/uint64_t）
- `gen_mask(width)` — 位宽→掩码字面量（如 0xFFULL）
- `byte_count(width)` — 位宽→字节数

## 元件 JSON 序列化 / 反序列化注意事项

序列化流程：`toJson()` → `jc["type"] = c->typeName()` + `jc["params"] = c->exportParams()` → `fromJson()` → `factory.create(type, name, params)`

实现新元件必须保证以下几点，否则 JSON 往返会损坏：

### 1. typeName() == 工厂注册键

基类构造的第二个参数（`_type_name`）必须和 `registerType` 第一个参数**逐字一致**，不能附加位宽等参数：

```cpp
// ✅ 正确：两边都是 "abs"
Abs(name, bw) : CombinationalComponent(name, "abs")  ...
f.registerType({"abs", ...}, ...)

// ❌ 错误：typeName="abs8"，工厂键="abs"，反序列化找不到
Abs(name, bw) : CombinationalComponent(name, std::format("abs{}", bw)) ...
```

`typeName()` 是类型标识，不是实例标识。参数应通过 `setParam()` 记录。

### 2. setParam() 覆盖所有构造参数

`exportParams()` 只导出 `setParam()` 设过的值。缺的参数不会被序列化，反序列化出来的元件丢失行为：

```cpp
// ✅ 正确
MinMax(..., is_max, signed) {
    setParam("bit_width", ...);
    setParam("mode", is_max ? "max" : "min");
    setParam("signed", signed ? "1" : "0");
}
```

### 3. 默认值三方对齐

`registerType` 的 ParamMeta::default_val、工厂 lambda 中 `_gi()` 的默认值、C++ 构造函数默认参数三者必须一致：

```cpp
// ParamMeta:  {"bit_width", "8"}     → JSON 缺省时填 "8"
// _gi():      _gi(p, "bit_width", 8) → 解析为 int，缺省 8
// 构造函数:   Abs(name, bit_width)   → 没有默认值的可以不管
```

### 4. valid_values 枚举与实现代码同步

```cpp
f.registerType({"edgedet",
    {{"edge", "rising", {{"rising"}, {"falling"}, {"both"}}}},
    ...})
```

`fromJson` 会校验参数值。JSON 里写了列表外的值直接报错。构造函数里的 `if/else` 字符串映射逻辑要和这里对齐。

### 5. 引脚名不可变，顺序不可变

序列化按 `pin->name()` 写连接关系，反序列化按 name 查找引脚。`addInput(name, bw)` 的 name 一旦定下就不能改，否则旧 JSON 文件回读失败。

工厂 Meta 里的 `input_pins`/`output_pins` 用于生成 schema，也应和实际引脚名一致。

### 6. 位宽参数不可少

所有 `addInput()`/`addOutput()` 调用的位宽必须来自 `setParam()` 记录过的参数。反序列化时先调构造函数（参数来自 params map），再 connect。构造函数里 addInput 用的位宽值如果没 setParam，将来 JSON 导出的参数就不完整。

### 7. 检查清单

新增元件后，写一个 JSON 往返测试确保序列化正确：

```cpp
// 构建 → 导出 JSON → 反序列化 → 检查参数和连接一致
std::string json = c.toJson();
auto c2 = Circuit::fromJson(json);
EXPECT_EQ(c2->components()[0]->typeName(), original_type_name);
```
