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

### 用自己的 Verilog 跑仿真（不用重编译 dcs）

安装版自带 `verilog2dll.py`（在 `<prefix>/share/dcs/tools/`）。一个或多个 `.v`
（顶层 + 子模块）一起传给脚本：

```bash
PY=<prefix>/share/dcs/tools/verilog2dll.py
python $PY top.v sub1.v sub2.v -o my_demo     # 顶层由 AST 自动识别，--top 可显式指定
cmake -S my_demo -B my_demo/build
cmake --build my_demo/build
<prefix>/bin/dcs my_demo/top.json -t 20 -m    # 所有端口已接成同名线网，可直接冒烟
```

生成的 `my_demo/top.json` 是冒烟电路：把里面的 `constant`/`clkgen` 换成你的激励即可
（端口名、位宽、组合/时序都从 Verilator AST 读取）。改 Verilog 后重跑第一条命令；
`<name>_wrapper.cpp` 已存在时脚本会保留（删掉才会按新端口重新生成）。

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

### 把 Verilog 模块打包成运行时 DLL 元件（不重新编译 dcs）

不想把模块编进库时，`tools/verilog2dll.py` 会生成一个独立的 CMake 工程，
由 `dll` 元件在运行时加载动态库：

```bash
python tools/verilog2dll.py my_top.v -o verilog_dll/my_top
cmake -S verilog_dll/my_top -B verilog_dll/my_top/build
cmake --build verilog_dll/my_top/build
./build/RelWithDebInfo/dcs verilog_dll/my_top/my_top.json -t 10 -m   # 冒烟
```

输出目录里的内容：

- `obj_dir/`：Verilator 模型和 AST（保留，便于排查/重建）
- `<name>_wrapper.cpp`：包装层，实现 `include/dcs/dll_interface.h`，可以直接改；
  已存在时脚本不会覆盖
- `CMakeLists.txt`：独立工程，编译器和生成器自选（MSVC / clang / gcc 都行），
  产物固定输出到 `dist/`；单配置生成器默认 Release
- `<name>.json`：把所有端口接成同名线网的冒烟电路，`path` 已经指向 `dist/<name>.dll`

在电路里引用它：

```json
{"type": "dll", "name": "u0", "params": {"path": "dist/my_top.dll"},
 "connections": {"a": "a", "b": "b", "y": "y"}}
```

`dcs --dll-info dist/my_top.dll` 可以先看 DLL 导出的引脚。限制：DLL 接口没有
三态使能通道，`inout` 端口按常驱动处理；模块 parameter 只能使用源码默认值。

### 安装与 find_package

```bash
cmake --install build --prefix <prefix>
```

安装内容：`bin/` 下的 `dcs`，`lib/` 下的 dcs_lib，`include/dcs/` 头文件，
`share/dcs/examples/`，`share/dcs/tools/`（`verilog2dcs.py`、`verilog2dll.py`、
`verilog_common.py`）。Windows 的 TCC 构建还会安装 `libtcc.dll` 和
`bin/shim/tccdefs.h`（TCC 运行时必须能读到这个文件）；dcs 自己的 shim 头已经编进
dcs_lib，不随安装复制。

下游工程可以直接用：

```cmake
find_package(dcs CONFIG REQUIRED)
target_link_libraries(app PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,dcs::dcs_lib>)
```

静态库必须整库归档，否则元件自注册对象会被链接器丢掉；`DCS_SHARED_LIB=ON` 的共享库
直接链 `dcs::dcs_lib` 即可。config 会自动 `find_dependency` nlohmann_json 和 elfio
（公开头文件里用到），下游环境需要有这两个包（vcpkg 或系统安装）。

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

# 校验电路或蓝图（只加载、展开、静态检查，不编译）
./build/RelWithDebInfo/dcs --check circuit.json
./build/RelWithDebInfo/dcs --check library/adder.json

# 列出蓝图定义以及它们来自哪个文件
./build/RelWithDebInfo/dcs --list-definitions circuit.json

# 递归列出目录下的蓝图元件（--depth 限制子目录层数，--json 给机器读）
./build/RelWithDebInfo/dcs --list-library library --depth 2
./build/RelWithDebInfo/dcs --list-library library --json

# 从元件注册表生成 schema（给 IDE 提示用）
./build/RelWithDebInfo/dcs --dump-schema > schemas/component.schema.json

# 额外的蓝图查找路径（可重复），另外还有 DCS_LIB_PATH（PATH 风格）
./build/RelWithDebInfo/dcs circuit.json -I blueprints -I ../shared/blueprints
```

## 作为子目录集成

```cmake
set(DCS_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_VERILOG OFF CACHE BOOL "" FORCE)
add_subdirectory(digital_circuit_simulation)

target_link_libraries(my_app PRIVATE dcs_lib)
```

可变状态放在 `dsc::Context` 里（LLVM 的 `LLVMContext` 那种形式）：蓝图库（定义 + 查找
路径）和 JIT 头文件目录。`Circuit` 不调用 `setContext()` 时用 `Context::global()`；
`Circuit::fromJson(json, err, base_dir, &ctx)` 可以把这次加载绑定到指定上下文，所以
一个进程里可以同时存在多套互不相关的电路，即使它们定义了同名蓝图也不会互相覆盖。
元件类型注册表（`ComponentFactory`）仍是进程级的：登记发生在静态初始化阶段，只有
"这次加载出来的状态"按上下文隔离。

```cpp
dsc::Context ctx;
ctx.library().addSearchPath("blueprints");
std::string error;
auto circuit = dsc::Circuit::fromJson(json, error, dir, &ctx);
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
| `parity` | 奇偶校验位生成 | `bit_width`, `mode`（even/odd） |

### 浮点（IEEE 754）

| 类型 | 说明 | 参数 |
|------|------|------|
| `fadd`, `fsub`, `fmul`, `fdiv` | 浮点四则运算 | `precision`（32/64） |
| `feq`, `fne`, `flt`, `fgt`, `fle`, `fge` | 浮点比较 | `precision`（32/64） |
| `f2i`, `i2f` | 浮点/整数互转 | `precision`, `int_width`, `signed` |
| `fconst` | 浮点常量 | `precision`, `value`（如 "3.14"） |
| `adc` | 模数转换：float 电压量化到 N 位数字量，越界饱和 | `bit_width`（1~32）, `vref` |
| `dac` | 数模转换：N 位数字量线性映射到 `[0, vref]`，输出是 float 位模式 | `bit_width`（1~32）, `vref` |

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
| `ringcounter` | 环形/约翰逊计数器 | `length`, `johnson`（0/1） |
| `sync2ff` | 跨时钟域两级同步器 | `bit_width` |
| `debounce` | 按键去抖：输入稳定 `stable_ticks` 个时钟沿后才更新 | `bit_width`, `stable_ticks` |

### 存储器

| 类型 | 说明 | 参数 |
|------|------|------|
| `memory` | 带读写延迟的 RAM | `addr_width`, `data_width`, `read_latency`, `write_latency` |
| `rom` | ROM（hex / raw 字节 / 文件 mmap） | `addr_width`, `data_width`, `source_type`（hex/file）, `filepath`, `use_mmap` |
| `mpram` | 多端口 RAM（1~4 读 + 1~2 写，含 rd_valid 流控） | `addr_width`, `data_width`, `num_read_ports`, `num_write_ports`, `read_latency` |
| `fifo` | 同步 FIFO | `data_width`, `depth`（2 的幂）, `has_rst` |
| `async_fifo` | 双时钟域异步 FIFO（格雷码指针 + 两级同步器） | `bit_width`, `depth` |
| `regfile` | 多读口寄存器堆（组合读、时钟沿写） | `bit_width`, `depth`, `num_read_ports`, `num_write_ports` |

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
| `wb_dcache` | 写直通数据缓存，带总线监听 | `lines`, `base`, `size` |
| `reg_bridge` | Wishbone 从设备 → 通用寄存器接口（addr/wdata/we/en） | — |
| `gpio` | 32 位 GPIO，DIR/OUT/IN 三个寄存器 | — |
| `pwm` | 单通道 PWM（PERIOD/DUTY/CTRL） | — |
| `rtc` | goldfish 风格实时时钟（纳秒），带闹钟中断 | `ns_per_tick` |
| `watchdog` | 看门狗定时器（CTRL/TIMEOUT/FEED/STATUS） | — |
| `virtio_blk` | virtio-blk，镜像来自宿主文件或电路里的 ROM | `path` |
| `virtio_blk_mem` | virtio-blk，介质是设备自带存储 | `size_kb` |
| `virtio_net` | virtio-net，后端由 `backend` 选择（`none`/`slirp`） | `backend`, `backend_opts` |
| `virtio_rng` | virtio-rng，随机数在 JIT 里生成 | — |
| `virtio_input` | virtio-input（evdev），宿主用 `pushInputEvent` 喂事件 | — |

CPU 支持 M/S/U 特权级、Sv32/Sv39 分页、Zicsr/Zifencei、A/F/D/Zfh/Zba/Zbb/Zbc/Zbs/Zicond
扩展，以及 Sdtrig 硬件断点（mcontrol 子集）。

### 工具

| 类型 | 说明 | 参数 |
|------|------|------|
| `print` | 打印整数线网值 | `bit_width` |
| `fprint` | 打印浮点线网值 | `precision`（32/64） |
| `dll` | 外部 DLL 元件 | `path` |
| `composite` | 可复用子电路 | `definition`（蓝图名） |
| `assert` | 仿真断言：时钟沿上 `cond=0` 时报一次 | `message`, `max_reports` |
| `display` | 显示件（LED/数码管/文本），把值交给宿主 GUI | `bit_width`, `kind`（led/seg/text）, `digits` |
| `bcd2seg` | BCD/十六进制到七段码译码（bit7 是小数点） | `hex_mode`（0/1）, `active_low`（0/1） |
| `wallclock` | 日历时钟，虚拟累加或直接同步宿主时间 | `ns_per_tick`, `base_epoch`, `sync_host`（0/1） |
| `uart_in` | 串口输入：宿主 stdin 按 8N1 发到 UART.rx | `baud_div` |
| `uart_feed` | 串口输入：宿主程序 push 字节（不碰 stdin） | `baud_div` |
| `uart_term` | 串口终端接收端：8N1 电平解成字节 | `baud_div` |

### JIT 优化级别

生成的 C 代码在交给 JIT 之前会跑一组只做精简的 IR pass（mem2reg、early-cse、
instcombine、simplifycfg、GVN、SCCP、DCE），默认开启。环境变量可以覆盖：

| 值 | 行为 |
|----|------|
| `basic`（默认） | 只跑不会让代码变大的精简 pass |
| `none` | 一条 pass 都不跑，用于排查生成代码本身的问题 |
| `inline` | 额外跑 Inliner。实测没有稳定收益：`circuit_tick` 被内联成巨型函数，icache 压力抵消了内联收益，JIT 编译时间也翻倍 |

```bash
DCS_JIT_PASS_SET=inline ./build/RelWithDebInfo/dcs_bench
DCS_JIT_PASS_SET=none ./build/RelWithDebInfo/dcs_bench
```

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
./build/RelWithDebInfo/dcs_soc examples/soc/riscv_linux.json --elf images/boot.elf --disk images/disk.img --forever
```

整机仿真用单独的 `dcs_soc` 入口：加载 ELF、串口接宿主键盘、按 tohost 判定结果
这些是计算机仿真特有的东西，不塞进纯电路仿真的 `dcs`。

### 给自定义 CPU 装载镜像（不用内置 RISC-V 核）

`dcs_soc` 不要求电路里一定有 `RiscVCPU`：没有 `--elf` 时跳过 ELF/入口/tohost 处理，
用 QEMU loader 式的参数自己布置内存和入口：

```bash
dcs_soc my_cpu.json \
  --load fw.bin@0x80000000 \
  --load dtb.bin@0x81000000 \
  --set start_pc=0x80000000 \
  -t 100000
```

- `--load FILE@ADDR`：原始字节放到第一个 `WishboneMemory`（可重复）。地址按存储器容量
  别名回绕，和 ELF 装载一致——`0x80000000` 这类高地址会落到低地址窗口；后面的 `--load`
  覆盖前面重叠的区域。文件比存储器还大直接报错；
- `--set NET=VALUE`：`init()` 之后、第一拍之前按线网名写值（十六进制，最多 128 位）。
  写的是没有驱动源的输入线网；仿真器不会每拍清零 `_w[]`，值会一直保持到有人改它；
- 入口如何生效取决于你的 CPU：有 `start_pc` 输入就接到一根线网、用 `--set` 写；
  复位向量硬连在核里的话，把镜像装到那个地址即可。

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

参数值也可以是嵌套对象，宿主侧后端就是靠它携带专有选项：
`"params": {"backend": "slirp", "backend_opts": {"hostfwd": "..."}}` 加载时拍平成
`backend_opts.hostfwd` 参数键（参数表仍然是 string→string），后端按前缀取自己那段。

### 元数据与蓝图文件

两种 JSON 都可以带可选的元数据：

```json
"$meta": { "kind": "circuit", "format": 1, "author": "...", "description": "..." }
```

`kind` 是 `circuit`（没有 `$meta` 时的默认值，老文件照常加载）或 `blueprint`；
`format` 是数据格式版本，比程序支持的新会直接拒绝；`author` / `description` 可选，
JSON 往返后会保留。

可复用的子电路可以单独存成蓝图 JSON，顶层电路在 `definitions` 里按路径引用。
相对路径先按引用它的文件所在目录找，再按 `-I/--lib-path` 给的目录顺序找
（和 C++ 头文件搜索一样）：

```json
{
  "$meta": { "kind": "circuit", "format": 1 },
  "definitions": {
    "not_gate": "library/not_gate.json"
  },
  "components": []
}
```

蓝图文件用 `"$meta": {"kind": "blueprint"}` 标记，内容可以是 `definitions` 映射，
也可以把单个定义直接写在根上（名字取 `$meta.name`，没写就用引用处的键）。顶层文件里的
内联定义优先于文件里的同名定义；蓝图之间循环引用会被拒绝。示例见
`examples/combinational/composite_library.json` 和项目根的 `library/`。

蓝图可以带参数：定义里写 `"params": {"bit_width": "8"}` 给默认值，实例用同名参数覆盖，
配方参数、实例名、线网名和端口位宽里的 `${...}` 会在展开时求值。端口位宽写成
`{"net": "a", "width": "${bit_width}"}`。

`${...}` 里可以写参数名、整数四则运算和拼接：

```
template := ( TEXT | "${" expr "}" )*
expr     := add
add      := mul ( ("+" | "-") mul )*
mul      := unary ( ("*" | "/") unary )*
unary    := ("+" | "-") unary | primary
primary  := INTEGER | IDENT | "(" expr ")"
INTEGER  := [0-9]+
IDENT    := [A-Za-z_][A-Za-z0-9_.]*
```

- `+` 两边都是整数时做加法，否则按字符串拼接；`u_${i}`、`${prefix}_${suffix}` 这种直接相邻也会拼接；
- `-` `*` `/` 只接受整数，`/` 向零截断，除零/溢出会报错；
- 单独引用参数时保留原文本（位宽写 `"007"` 不会被改成 `"7"`），只有参与算术才按整数处理；
- 求值结果不做范围钳制，位宽仍是 1~128、由使用它的地方校验；
- 没有循环展开，端口数量随位宽变化的结构请用总线端口或内置的 `adder`/`register` 这类带 `bit_width` 的元件。

`schemas/circuit.schema.json`、`schemas/blueprint.schema.json` 只给编辑器提示用，
不参与任何校验，文件里不引用它们也没关系。`schemas/component.schema.json` 由元件
注册表生成：`dcs --dump-schema > schemas/component.schema.json`。

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
