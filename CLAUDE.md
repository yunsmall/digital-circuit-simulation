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

两个后端的取舍（实测：61 个官方 ELF，同机同构建配置）：

| 后端 | 总耗时 | 说明 |
|------|--------|------|
| TinyCC | ~370 ms | 编译极快，生成代码的优化程度一般 |
| LLVM | ~1780 ms | Clang 前端慢，但生成代码运行更快 |

测试程序每个只跑几千拍，编译开销占大头，所以 TCC 快得多；长时间仿真（几十万拍以上）反过来是 LLVM 占优。
改前端实现时两个后端都要构建一遍——TCC 侧的 `tcc.h` 是内部头，发行包只带公开的 `libtcc.h`。

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

## 性能基准

`benchmarks/` 下有六个 ELF 负载场景和两个纯电路场景，用来防止性能回归。

```bash
bash benchmarks/build.sh          # 交叉编译基准 ELF（clang，rv64gc）
./build/RelWithDebInfo/dcs_bench  # 默认每个场景 4000 万 tick、重复 3 次取最小值
```

`--ticks N` 改拍数，`--repeat R` 改轮数，也可以直接传某个 ELF 路径只跑那一项。
表格里的"完成周期"是程序写 tohost 所需的周期数，比 tick 吞吐更能反映真实速度。

驱动时钟优先用 `circuit->tickCycles(clk_net_id, n)`（循环在 JIT 内），
比每拍 `setWire` + `tick` 快；`clkgen` 元件约慢 7%，只在需要分频/多时钟域时用。

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

- `gen_read_wire(net_id, pin_w, net_w, var_name)` — 从 `_w[]` 读取值的 C 语句，变量自动声明并 `= 0` 初始化。生成形如：
  `uint64_t _adr = 0; dcs_memcpy(&_adr, _w[42], 1); _adr &= 0xFF;`
- `genOutputWrite(out_idx, val_expr, pin_w)` — 写 `_c_out_<comp_id>_<out_idx>` 的 C 语句。生成形如：
  `dcs_memcpy(_c_out_0_0, &_dout, 4); dcs_memset(_c_out_0_0 + 4, 0, 12);`
- `c_int_type(width)` — 位宽→C 类型名（uint8_t/uint16_t/uint32_t/uint64_t）
- `gen_mask(width)` — 位宽→掩码字面量（如 0xFFULL）
- `byte_count(width)` — 位宽→字节数
- `funcName_seq()` / `funcName_comb()` — 返回当前元件的 seq/comb 函数名

### circuit_tick 执行顺序与 _c_out_ → _w[] 流水线

每个 tick 内 `circuit_tick` 按以下顺序执行：

```
阶段1: 组合逻辑（按拓扑序，每个 comb 执行后立即将其 _c_out_ 传播到 _w[]）
阶段2: 总线决议 + 时序输出传播（seq 的 _c_out_ → _w[]）
阶段2b: 反复重跑"输入接了总线"的组合元件 + 重新决议总线，直到总线值不再变化
阶段3: 所有时序逻辑函数（seq）
```

- **comb 输出同 tick 可见**：阶段1 内，每个组合元件执行后立即 `_c_out_` → `_w[]`，下游组合元件读到当前拍结果。组合链单拍穿透。
- **seq 输出跨 tick 可见**：seq 在阶段3 写 `_c_out_`，下一 tick 的阶段2 才进 `_w[]`。跨时序元件的信号传播有 1 tick 延迟。

**组合元件不能假设自己一个 tick 只跑一次。** 只要它的输入接了多驱动总线，阶段1 里读到的是
"总线还没决议"的值，必须在阶段2b 补跑。补跑会改动总线，又会影响下一级读总线的元件，
所以阶段2b 是一个迭代到不动点的循环（上限为元件数+1，防止电路出现自激振荡时死循环）。
新增组合元件时如果读的是总线线网，务必保证输出对输入是纯函数，否则迭代不会收敛。

**组合环在编译期就拒绝**：`Circuit::check()` 会做环路检测并报"检测到组合逻辑环路"，
非门自接这类电路根本进不到运行期。通过三态总线绕成的环检测不出来，靠阶段2b 的迭代上限兜底。

**铁律：输出必须走 `genOutputWrite`（写 `_c_out_`），严禁直写 `_w[]`**。

### JIT 代码生成的两种实现模式

**模式 A：纯 JIT**（LogicGate, DFlipFlop, ROM, Memory, FIFO, Adder, BarrelShifter 等大多数元件）

全部逻辑内联在生成的 C 代码中，无 C++ 回调。读 `_w[]` → 运算 → 写 `_c_out_`。参考 ROM 或 DFlipFlop：

```cpp
// genStateDecl: 声明 static 变量（状态）
code += std::format("static uint64_t _rom_addr_{0}[{1}];\n", id, _rd_stages);
// genInitCode: 初始化
code += "    dcs_memset(_rom_addr_0, 0, sizeof(_rom_addr_0));\n";
// genFuncDef_seq: 完整 C 逻辑
code += std::format("static void {}() {{\n", funcName_seq());
code += "    " + gen_read_wire(adr_net_id, 8, 8, "_adr") + "\n"; // 读输入
code += "    ... // 内部计算\n";
code += "    " + genOutputWrite(0, "_result", 32) + "\n";         // 写输出
code += "}\n";
```

**模式 B：C++ 回调**（CLINT, WishboneMemory, WishboneInterconnect, Print）

JIT 代码只做引脚搬运（读 `_w[]` → 打包 → 调 C++ 函数 → 解包 → `genOutputWrite`），实际逻辑在 C++ 回调中实现。需要：

1. `extraJitSymbols()` 把 C++ 函数指针暴露给 JIT
2. `genStateDecl()` 把 C++ 状态对象指针暴露给 JIT
3. `genFuncDef_seq()` 生成打包/解包/回调代码

参考 WishboneMemory.cpp 第 76-127 行或 CLINT.cpp 第 57-89 行。

选择标准：逻辑简单用纯 JIT（少一层打包开销），复杂状态机/MMIO 用 C++ 回调（方便调试）。

### 跨元件时序计算（适配器/桥接类元件的关键）

comb 输出同 tick 可见，seq 输出有 1 tick `_c_out_`→`_w[]` 延迟。

**seq ↔ seq 通信**（如 WishboneROMAdapter → ROM，两者都是 seq）：

```
总延迟 = 1（适配器 seq 输出→_w[]） + ROM内部延迟 + 1（ROM seq 输出→_w[]→适配器读）
       = ROM内部延迟 + 2
```

例如 ROM 的 `read_latency=2`，适配器需要 `wait = 2 + 2 = 4` tick 从 stb 到 ack。

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

## 提交

有较大的更新（新增元件、目录或 API 结构调整、行为变更、大规模重构、文档同步）时，
自行按改动内容提交，不必每次询问。提交信息不要加 `Co-Authored-By` 之类的协助者署名；
只有明确要求时才 push。小修小补可以攒着和下一次大更新一起提交。

