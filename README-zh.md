# 数字电路仿真

[English](README.md)

基于 JIT 编译的数字电路仿真框架。从元件构建电路，用 JSON 描述，通过 LLVM/Clang ORC JIT 或 TinyCC 编译为原生机器码高速仿真。

## 特性

- **70+ 内建元件**：门、触发器、寄存器、计数器、加减乘除、比较器、浮点、RAM、ROM、FIFO、桶形移位器、符号扩展、优先级编码器、多端口 RAM，以及 RISC-V 软核、Wishbone 总线、UART/CLINT/PLIC 等平台元件
- **JIT 编译**：电路 → C 代码 → 机器码，内存中完成
- **双后端**：LLVM/Clang ORC JIT（默认）或 TinyCC（轻量替代）
- **JSON 导入/导出**：完整电路描述，可往返
- **复合元件**：共享蓝图的子电路（定义一次半加器，实例化十次）
- **DLL 元件**：运行时加载外部 Verilog/Verilator/C 模块
- **Verilator 集成**：Verilog → C++ → DLL → 仿真（可选）
- **可配置位宽**：1~128 位 / 线网
- **组合环路检测**：三色 DFS + 报告
- **参数校验**：非法值在 JSON 加载时报错
- **gtest 测试**：32 个测试目标，覆盖全部元件类型与官方 riscv-tests 用例

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

### 从 Verilog 生成元件

`tools/verilog2dcs.py` 把一个 Verilog 模块直接变成 dcs 元件（Verilator 模型 + 生成的包装代码），
不需要 DLL：

```bash
python tools/verilog2dcs.py verilog_test/adder8.v --type gen_adder8
cmake -B build ...            # 新增文件后要重新配置，GLOB 才扫得到
cmake --build build --parallel 6
```

模块的端口会变成 dcs 引脚，端口方向、位宽和"有没有沿触发的 always 块"都从
Verilator 导出的 AST 里读，所以时钟叫什么名字都行。端口位宽上限 128 位；
unpacked 数组端口会按维展开成独立引脚，`inout` 用 Verilator 官方的
`--pins-inout-enables` 拆成输入 `pad_i` + 三态输出 `pad`，接同一条总线即可双向通信。
生成的元件每拍调一次 Verilator 的 `eval()`，模型的寄存器状态由 Verilator 自己维护。

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
| `abs` | 绝对值 | `bit_width` |
| `min`, `max` | 最值 | `bit_width`, `signed`（0/1） |
| `redand`, `redor`, `redxor` | 归约与/或/异或 | `bit_width` |
| `lsl`, `lsr`, `asr` | 移位（移位量来自输入引脚） | `bit_width` |
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
| `feq`, `fne`, `flt`, `fgt`, `fle`, `fge` | 浮点比较 | `precision`（32/64） |
| `f2i`, `i2f` | 浮点/整数互转 | `precision`, `int_width`, `signed` |
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
| `clkdiv` | 时钟分频 | `divisor` |
| `edgedet_rise`, `edgedet_fall`, `edgedet_both` | 边沿检测 | — |
| `delay` | N 级延时线 | `bit_width`, `depth`（1~256）, `use_clock`（0/1） |

### 存储器

| 类型 | 说明 | 参数 |
|------|------|------|
| `memory` | 带读写延迟的 RAM | `addr_width`, `data_width`, `read_latency`, `write_latency` |
| `rom` | ROM（hex / raw 字节 / 文件 mmap） | `addr_width`, `data_width`, `source_type`（hex/file）, `filepath`, `use_mmap` |
| `mpram` | 多端口 RAM（1~4 读 + 1~2 写，含 rd_valid 流控） | `addr_width`, `data_width`, `num_read_ports`, `num_write_ports`, `read_latency` |
| `fifo` | 同步 FIFO | `data_width`, `depth`（2 的幂）, `has_rst` |

### RISC-V 平台元件

| 类型 | 说明 | 参数 |
|------|------|------|
| `riscv_cpu` | RISC-V 软核，Wishbone 主设备 | `xlen`（32 或 64） |
| `wishbone_mem` | Wishbone 从设备存储器 | `addr_width`（1~24）, `data_width`（8/16/32） |
| `wishbone_rom_adapter` | ROM 的 Wishbone 适配器 | `addr_width`, `data_width`, `read_latency` |
| `wishbone_ic` | Wishbone 地址解码/路由 | `segments`（如 `0:65536,0x80000000:65536`）, `num_masters` |
| `clint` | CLINT：mtime/mtimecmp/msip | — |
| `plic` | PLIC 中断控制器 | `num_sources` |
| `uart` | 16550A 兼容 UART | `baud_div` |
| `spi` / `i2c` | SPI / I2C 主设备 | `clkdiv` |
| `dma` | 存储到存储的 DMA | — |
| `axi_lite_bridge` | AXI-Lite ↔ Wishbone 桥 | — |
| `wb_cache` | Wishbone 缓存 | `lines` |
| `virtio_mmio` | virtio-mmio（modern v2）设备：从端口接 CPU，主端口经总线访问 virtqueue | `device`（blk/console）, `path`（blk 镜像文件，留空则用电路里的 ROM） |

CPU 支持 M/S/U 特权级、Sv32/Sv39 分页、Zicsr/Zifencei、A/F/D/Zfh/Zba/Zbb/Zbc/Zbs/Zicond
扩展，以及 Sdtrig 硬件断点（mcontrol 子集）。

### 工具

| 类型 | 说明 | 参数 |
|------|------|------|
| `print` | 打印整数线网值 | `bit_width` |
| `fprint` | 打印浮点线网值 | `precision`（32/64） |
| `dll` | 外部 DLL 元件 | `path` |
| `composite` | 可复用子电路 | `definition`（蓝图名） |

## 性能基准

`benchmarks/` 提供六个 ELF 负载场景（整数 ALU、访存、分支、乘除、双精度浮点、混合）
和两个纯电路场景（256 级门链、256 个 D 触发器），用于观察不同负载下的仿真吞吐：

```bash
bash benchmarks/build.sh             # 交叉编译基准 ELF 到 benchmarks/out
./build/RelWithDebInfo/dcs_bench     # 默认每个场景 4000 万 tick，重复 3 次取最小值
```

输出包含 JIT 编译耗时、每秒 tick 数、完成周期数与 icache 命中率；
`--ticks N` 改拍数，`--repeat R` 改轮数，`--dir` 指定 ELF 目录，也可直接传 ELF 路径。

驱动时钟有两种方式，基准里都能看到：

- `circuit->tickCycles(clk_net_id, n)` — 在 JIT 内部连跑 n 个周期，最省
- 放一个 `clkgen` 元件让它自己走时钟，每拍调 `circuit->tick()` — 慢约 7%，
  适合需要分频或多时钟域的电路

参考数据（Windows / clang 22，RelWithDebInfo）。"完成周期"是跑完整个程序所需的
时钟周期数，比 tick 吞吐更能反映真实速度：

| 场景 | 周期数 | 耗时 | 每指令周期 |
|------|--------|------|-----------|
| alu（整数） | 3202 万 | 1.06 s | 1.0 |
| branch（分支密集） | 2668 万 | 0.97 s | 1.0 |
| fp（双精度） | 3202 万 | 1.15 s | 1.0 |
| mem（访存） | 462 万 | 0.13 s | 1.0 |
| mixed（混合） | 1179 万 | 0.35 s | 1.0 |
| muldiv（乘除） | 5601 万 | 2.72 s | 3.5 |

icache 命中后每条指令固定 1 个周期；muldiv 更高是因为 64 位除法本身要多拍。
与最初版本相比（同机同配置实测）：alu 9.22 s → 1.06 s，总体约 5~8 倍。

## 跑 Linux

`linux/` 目录提供一套完整的内核启动流程：把 Linux 6.12 的 Image、设备树和
initramfs 打包成**单个 ELF**，由 dcs 的 RISC-V CPU 直接启动到用户态；
换成带 busybox 的 initramfs 就能进到可交互的 shell。

```bash
bash linux/build-boot.sh /tmp/initrd-busybox.cpio.gz
./build/RelWithDebInfo/dcs_soc examples/jsons/riscv_linux.json --elf images/boot.elf --forever
```

整机仿真用单独的 `dcs_soc` 入口：加载 ELF、串口接宿主键盘、按 tohost 判定结果
这些是计算机仿真特有的东西，不塞进纯电路仿真的 `dcs`。

依赖、分步编译方式，以及中途踩过的硬件语义坑（PLIC 的 enable/pending 是位图寄存器、
UART 的 `reg-shift`、CLINT 的 mtimecmp 复位值等）都在
[`linux/README.md`](linux/README.md)。

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
- 线网最大位宽 128 位（内部 16 字节/槽）

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
**常规元件（全部逻辑写在生成的 C 里）**

- `gen_read_wire(net_id, pin_w, net_w, var)` — 读线网值（变量自动 `= 0`）
- `genOutputWrite(out_idx, val, pin_w, drive = "true")` — 写输出引脚。非三态输出直接写 `_w[]` 并清零高位；
  三态输出写 `_c_out_` 并把使能置成 `drive`。**新元件默认用这个**
- `gen_write_wire(comp_id, out_idx, val, pin_w)` — 直接写 `_c_out_`（只有需要自己控制三态时序时才用）

**回调型元件（JIT 只搬引脚，逻辑在 C++ 回调里）专用的高性能路径**

这三个绕过了 `genOutputWrite` 的高位清零约定，只有满足前提的回调型元件能用：

- `genInputPtrArray(arr)` — 输入线网的 static 指针表，回调直接读，省掉每拍打包
- `genOutputPtrArray(count, clear)` — 输出先写 `_obuf`，再经 `genOutputWrite` 搬到线网（稳妥，多一轮拷贝）
- `genOutputWirePtrArray(arr)` — 输出由回调**直接写 `_w[]`**（极限性能，省一轮拷贝）。前提：
  输出线网单驱动、回调每拍写满所有输出、写的字节数不超过引脚宽度
- `c_int_type(width)` — 位宽 → C 类型（uint8/16/32/64_t）
- `gen_mask(width)` — 位宽 → 掩码字面量
- `byte_count(width)` — 位宽 → 字节数

## 许可证

MIT
