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

## slirp 网络后端（可选）

guest 里的 virtio-net 要访问宿主/互联网时用 libslirp，默认不编（CI 矩阵也不编它）：

```bash
vcpkg install libslirp
cmake -B build -DDCS_BUILD_NET=ON ...   # 其余参数同上
```

电路里用 `"backend": "slirp"` 选择后端。后端专有参数写在嵌套对象里，加载时拍平成
`backend_opts.<key>` 扁平参数键，元件按前缀摘出自己那段（见 `include/dcs/net/BackendOpts.h`）：

```json
"params": {
  "backend": "slirp",
  "backend_opts": { "hostfwd": "tcp:127.0.0.1:2223-10.0.2.15:23" }
}
```

元件不解释后端的键，换后端不改元件。宿主机连 guest 的完整流程见 `linux/README.md`。

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

### 从 Verilog 生成元件

`tools/verilog2dcs.py` 把一个 Verilog 模块变成 dcs 元件：调 `verilator_bin --cc` 生成模型，
用 `--json-only-output` 导出 AST，端口方向/位宽和时序信息全部从 AST 读（不要去解析
`V<top>.h` 的 `VL_IN/VL_OUT` 宏：那只是字符串快照，判定不了时序，也拿不到原始端口名）。

```bash
python tools/verilog2dcs.py verilog_test/adder8.v --type gen_adder8
cmake -B build_clang ...      # 新增目录后要重新配置，CMake 才扫得到
cmake --build build_clang --parallel 6
```

产物与约定：

- `include/dcs/components/verilog/<Name>.h`、`src/verilog_gen/<Name>.cpp`：元件本体。
  可执行文件里能用的类型名由 `--type` 指定（默认是模块名小写）。
- `verilog_gen/<top>/`：Verilog 源副本 + `dcs_component.txt`（记录元件类名）。
  CMake 按这个清单把元件 cpp 和 Verilator 模型编进同一个 target。
- 元件 cpp 故意放在 `src/verilog_gen/`：`src/components/*/*.cpp` 的 GLOB 扫不到这里，
  否则会被编进 dcs_lib 而拿不到 `V<top>.h` 的生成路径。
- **时序/组合看 AST，不看端口名**：模块里存在带 `edgeType`（POS/NEG）的 `SENITEM`
  才算时序元件（输出引脚标 sequential），否则是组合元件。时钟叫 `clock_50m`、`aclk`
  都无所谓，不要退回按名字猜——猜错会让有状态的模型被当成组合元件，阶段 2b 重跑时
  会把寄存器多推几拍。
- 端口位宽从类型表的 `range` 读，packed 多维按各维长度连乘；上限 128 位（dcs 线网槽是
  16 字节）。unpacked 数组端口按维展开成 `name_0`、`name_1`……，元素本来就是独立信号。
- `inout` 端口用 Verilator 官方的 `--pins-inout-enables`：顶层 inout 会被拆成
  `pad`(input)、`pad__out`、`pad__en`（按位使能）。生成器映射成输入引脚 `pad_i` +
  三态输出引脚 `pad`，两者接同一条总线；阶段 2b 会在"内部驱动值 → 总线决议 → 重新读入"
  之间迭代到不动点，所以 `din` 读到的就是总线上的实际值，和真实硬件一致。
  逐位混合使能在 dcs 里表达不了（三态使能是整根线的），按"任一位使能就整根线驱动"处理。
- 三态输出在环路检测里按"可断开"处理（见 `Component::getCombinationalDeps`）：双向端口
  天然成环，三态在 oe=0 时确实断开，经过三态总线的环靠阶段 2b 的迭代上限兜底。
- 引脚名用 AST 里的 `verilogName`，访问模型成员用 `name`：端口名撞 C++ 关键字时两者不同。
- 生成的元件直接继承 `Component`：Verilator 模型指针要放进 `genStateDecl`，
  而 `CombinationalComponent` 把 `genStateDecl` 标成了 `final`。
- Verilator 的端口是引用成员（`VL_IN8` 展开成 `CData &a`），不能按对象内偏移直接读写，
  必须走回调。
- 每拍每个元件只跨 JIT 一次：JIT 把输入打包进 static 数组，调 C++ 回调
  （写端口 → `eval()` → 读端口），再把输出写回线网。**必须每拍都 eval**——
  Verilator 模型内部记录上一拍的时钟值做边沿检测，在外面做边沿门控会让它
  永远看不到下降沿，此后所有上升沿都不成立。
- 所有模型共用一个 CMake target：`verilate()` 会把 `verilated.cpp` 这类运行时源文件
  塞进 target，一个模型一个 target 会重复定义符号。
- Windows 的 COFF 没有弱符号，`verilated.cpp` 弱引用 `sc_time_stamp()`，
  需要 `src/verilog_gen/verilated_stub.cpp` 提供定义，否则链接失败。

### 把 Verilog 打包成运行时 DLL 元件（不重编库）

`tools/verilog2dll.py` 走另一条路：生成一个独立 CMake 工程，把 Verilator 模型 +
包装层编成动态库，由 `dll` 元件运行时加载。脚本只生成工程，不调编译器：

```bash
python tools/verilog2dll.py my_top.v -o verilog_dll/my_top
cmake -S verilog_dll/my_top -B verilog_dll/my_top/build
cmake --build verilog_dll/my_top/build
./build/RelWithDebInfo/dcs verilog_dll/my_top/my_top.json -t 10 -m
```

产物在 `dist/`，生成的 JSON 里 `path` 已指好；`<name>_wrapper.cpp` 是可直接改的
包装层（脚本不覆盖已有文件）。Verilator 调用、端口/位宽/时序解析与
`verilog2dcs.py` 共用 `tools/verilog_common.py`，两条路的引脚定义必须一致。
已知限制：DLL 接口没有三态使能通道（`inout` 按常驱动处理），模块 parameter 只能
用源码默认值。

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

官方 riscv-tests 的 ELF 不在版本库里，`test_riscv` / `test_riscv64` 在找不到目录时会
`GTEST_SKIP`（CI 上会被静默跳过）。本地要跑官方套件先编：

```bash
bash tests/riscv-official/build.sh     # 产物 tests/riscv-official/out、out64
```

脚本用 clang 交叉编译（默认 `CLANG=clang`，可覆盖），链接必须走 lld——宿主的系统
`ld` 不认 riscv 目标。CI 由独立的 `riscv-elfs` 任务在 Ubuntu 上编一次，其余矩阵任务
下载 artifact 复用。

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

### 用 perf 把时间归因到元件

JIT 出来的机器码不在任何落盘文件里，perf 只能看到裸地址；libLLVM 自带的 jitdump
插件又依赖执行器里的 `llvm_orc_registerJITLoaderPerf*` 运行时符号，没链接它会静默
失败。可行的办法是让 JIT 把 IR 导出来，用同样的后端参数离线再编一遍，函数布局和
JIT 逐字节一致，再拿 `addr2line -i` 还原内联帧：

```bash
# 1. 生成带行号的 IR（DCS_JIT_DEBUG 打开调试信息，DCS_JIT_IR_DUMP 写文件）
DCS_JIT_DEBUG=1 DCS_JIT_IR_DUMP=/tmp/jit.ll dcs_soc circuit.json --elf boot.elf -t 500000000

# 2. 离线编成目标文件（参数必须和 JitBackend 里一致：-O2、PIC、CPU=x86-64）
llc -O2 -filetype=obj -relocation-model=pic -mcpu=x86-64 /tmp/jit.ll -o /tmp/jit.o
# 校验：nm 里 circuit_tick_cycle 的偏移要和运行时一致（两者之差就是代码基址）

# 3. perf 采样，再对 circuit_tick 范围内的地址跑 llvm-addr2line -f -i
perf record -F 999 -o perf.data -- dcs_soc ...
```

`DCS_DUMP_C=<路径>` 则把生成的 C 直接落盘，配合行号看热点最直观。

几个已经踩过的坑，别再试：

- clang 前端按 O2 跑完整流水线，元件函数基本都被内联进 `circuit_tick`，
  `DCS_JIT_PASS_SET` 只是在前端结果上再补一轮；用 `DisableLLVMPasses` 关掉前端能让
  元件函数独立，但整机慢十倍。
- 把"写 b 字节 + 补零到 8 字节"合成一次 8 字节整写，慢 13%：下游按自己的引脚宽度
  读同一根线，宽存储会让字节读等不到存储转发。
- 只补零到线网宽度（1 位线网就不用补）也慢 3%。
- `stb=0 就不读 cyc` 这类条件读慢 9%：把两个本来能并行的 load 串成了依赖链。

## 作为子目录集成到其他 CMake 项目

```cmake
set(LLVM_ROOT "..." CACHE PATH "")
set(DCS_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_VERILOG OFF CACHE BOOL "" FORCE)
add_subdirectory(digital_circuit_simulation)

target_link_libraries(my_app PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,dcs_lib>)
```

## Context：集成时的可变状态

`dsc::Context`（`include/dcs/Context.h`）按 LLVM 的 `LLVMContext` 那套组织：蓝图库
（定义 + 查找路径）和 JIT 头文件目录都放里面。`Circuit` 默认用 `Context::global()`，
`setContext()` 之后就用指定的那份；`Circuit::fromJson(json, err, base_dir, &ctx)` 把
加载绑定到上下文。一个进程里可以放多套电路，各自的同名蓝图不会互相覆盖。

新增的可变全局状态一律放进 Context，不要再写进程级单例。`ComponentFactory` 例外：
元件类型通过静态初始化自注册，只能进程级；它只登记"有哪些类型"，不保存加载结果。

## 技术栈

- C++23, CMake 4.2
- LLVM/Clang 22.1.7：Clang API 内存编译 C → LLVM IR，ORC JIT 执行
- TinyCC：libtcc 内存编译 C → 直接执行（轻量级替代后端）
- gtest 测试框架（vcpkg）
- std::format + 原始字符串字面量生成 C 代码
- 线网最大位宽 128 位（内部 16 字节/槽）

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
3. **自注册** — 在元件 .cpp 末尾用 `DCS_REGISTER_COMPONENT(meta, creator)` 登记，类型名、参数、引脚、分类、描述和实现都放在同一个文件里，不需要改动其它文件
4. **测试** — 参考已有结构（见下方）
5. CMake 的 `file(GLOB DCS_SOURCES ...)` 会自动拾取新文件，**但新增后需重新执行 `cmake -B build ...` 配置**，`add_dcs_test` 同理需重新配置

### 第三方引入本库时注册元件

自注册是公开头文件里的机制，外部项目不需要改本库任何文件：

```cpp
#include <dcs/ComponentFactory.h>

namespace {
DCS_REGISTER_COMPONENT(
    {"my_gate", {{"bit_width", "8"}}, {"in"}, {"out"},
     std::string("audio"), "第三方元件示例"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &p)
            -> std::unique_ptr<dsc::Component> {
        return std::make_unique<MyGate>(n, dsc::paramInt(p, "bit_width", 8));
    });
}
```

- **静态库符号丢弃**：如果元件编进第三方自己的静态库，链接器会丢掉没被引用的目标文件，
  自注册也就不会执行。要么对那个库用 `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`，要么提供一个
  被引用的入口函数。直接编进可执行文件则没有这个问题。
- **分类**：`Meta.category` 是字符串。内置的 10 类仍可用 `ComponentCategory` 枚举
  （构造函数会转成 `"gate"` 这类 key）；第三方想加自己的分类，直接给字符串 key，
  再用 `ComponentFactory::instance().registerCategory("audio", "音频")` 登记显示名，
  不需要改本库的枚举。没登记过的 key 在 `--list-types` 里原样显示。
- 参数解析可用 `dsc::paramInt` / `dsc::paramStr`；参数校验规则和引脚定义都在 Meta 里。

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

引脚描述来自外部资源、构造期拿不到的元件（复合元件查蓝图定义、DLL 元件读导出表），
不要在构造函数里硬凑：构造只记参数，覆盖 `Component::ensurePins(Context&, std::string&)`
建引脚。它必须幂等，失败时不能留下半套引脚。`Component::prepare()` 会先调 ensurePins
再调 `prepareImpl()`，所以 addComponent 成功后引脚一定齐全；`--pins` 这类工具也可以
只调 ensurePins 拿引脚，不必分配存储或读镜像。

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

**输出默认走 `genOutputWrite`（写 `_c_out_`），严禁随意直写 `_w[]`**。唯一合规的直写
路径是回调型元件的方案 B（`genOutputWirePtrArray`）：输出线网单驱动、回调每拍写满所有
输出、写入字节不超过引脚宽度，三条同时满足才行（见 `Component.h` 的说明）；CPU 生成
代码走的就是这条路。

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

**模式 B：C++ 回调**（Print, FloatPrint, UartFeed, UartIn, Dll, RiscVCPU, WishboneInterconnect）

JIT 代码只做引脚搬运（读 `_w[]` → 打包 → 调 C++ 函数 → 解包 → `genOutputWrite`），实际逻辑在 C++ 回调中实现。需要：

1. `extraJitSymbols()` 把 C++ 函数指针暴露给 JIT
2. `genStateDecl()` 把 C++ 状态对象指针暴露给 JIT
3. `genFuncDef_seq()` 生成打包/解包/回调代码

参考 WishboneMemory.cpp 第 76-127 行或 CLINT.cpp 第 57-89 行。

选择标准：**元件的主要逻辑一律塞进 JIT，哪怕状态机很复杂**。回调只留给"必须碰宿主"或"JIT 里根本写不出来"的操作——
文件 I/O、stdin、DLL 调用、CPU 指令执行。判断标准是"这段逻辑每拍都要跑吗、能不能用 C 写出来"，
而不是"写起来方不方便"。

**`extraJitSymbols()` 的符号名是全局按名字索引的**：多个元件注册同名符号时只有最后注册的
地址生效。同名同址（多个元件导出同一个全局函数）没问题；同名不同址会被 `Circuit::compile()`
直接拒绝，因为生成代码会调到另一家的回调、还按自己的布局解释对方的状态指针（virtio 设备
共用 `dcs_virtio_reset` 就这么崩过）。需要每个设备类各自的回调时，用类级别的固定名字
（如 `dcs_virtio_input_reset`），靠符号名区分实现，靠传进去的状态指针区分实例。

### JIT 代码的性能原则（TCC 后端同样要高效）

TCC 不做内联、常量传播、循环优化，生成的 C **写成什么样就是什么样**。所以代码生成要按
"编译器不会帮我优化"来写：

1. **构造期能算的全在生成期算**：源数量、位宽、分段表、掩码都展开成字面量。例如 PLIC 的
   claim 扫描按 `num_sources` 展开成 `num_sources` 条顺序比较，生成的 C 里没有循环。
2. **不要在生成的 C 里写循环**，需要重复就由生成器展开。TCC 不会展开循环，也不做循环不变量外提。
3. **按引脚位宽读线网**：`gen_read_wire` 只读 1/2/4/8 字节，不要固定读 16 字节。Clang 会把
   `dcs_memcpy` 内联成一条 mov，但 TCC 下它是 libc 调用，多读 15 字节就是实打实的开销。
4. **生成的元件函数一律是普通 `static` 函数**，不强制内联。LLVM 后端不开内联 pass
   （实测全部内联进 `circuit_tick` 后指令缓存被挤爆，吞吐掉 5~15%，JIT 编译时间
   还翻倍），元件之间保持普通函数调用。只有每拍被调几十次的小包装
   （`dcs_memcpy` / `dcs_memset` 这类线网搬运）才用 `DCS_FORCE_INLINE`。
5. **状态用 static 变量、按需读取**：不要在函数入口把整个状态结构体拷到局部变量，也不要每拍 memset。
6. **热路径上不要跨 JIT/C++ 边界**：每个元件函数每拍最多一次回调；能内联的判定（中断条件、比较、
   位运算）全部写在生成的 C 里。
7. **用简单表达式**：避免依赖编译器做分支合并、公共子表达式消除的写法。等价的两个判断宁可合并成
   一次位与再比较，也不要写成两层 `if`。
8. **输出默认走 `genOutputWrite`**；只有满足 `genOutputWirePtrArray` 三条前提的回调型元件
   （含 CPU）才允许直写 `_w[]`。组合元件不能假设自己一个 tick 只跑一次。

### 硬件建模原则：必须按真实电路实现

元件的行为、接口和时序都要和真实硬件一致，**禁止为了省事走"仿真专用"的捷径**。
典型反例：设备要读 guest 内存里的数据结构时，直接拿 `WishboneMemory::writableMemPtr()`
或 `IStorage` 的指针读写——真实芯片上根本没有这条路，设备只能通过总线主端口发起
读写事务。正确做法是像 `DMA`、`VirtioMmio` 那样：元件带主端口引脚（`m_adr_o` /
`m_dat_o` / `m_we_o` / `m_sel_o` / `m_stb_o` / `m_cyc_o` / `m_dat_i` / `m_ack_i`），
在 JIT 里实现事务状态机，总线上看到的就是一串真实读写。

区分清楚三类存储：

- **guest 内存**（`WishboneMemory` 里的 RAM）：只能走总线，禁止直接拿指针。
- **设备私有存储**：ROM 里的固件/镜像、宿主文件、FIFO 内部阵列。设备当然可以直接
  访问，这本来就是设备内部的事，不算绕过总线。
- **宿主侧工具**（ELF 装载、测试布置数据、`--dump-mem`）：可以直接用 `IStorage`
  接口，它们扮演的是"外部世界"，不是元件本身。

寄存器语义、握手（`stb`/`cyc`/`ack`）、字节使能（`sel`）、非对齐访问、中断电平和
真实硬件的对应关系也都要如实建模。如果确实需要走仿真专用的捷径（比如为了性能
在某个元件上直接访问内存），必须先明确说出来并得到用户确认，不能默默做。


### 跨元件时序计算（适配器/桥接类元件的关键）

comb 输出同 tick 可见，seq 输出有 1 tick `_c_out_`→`_w[]` 延迟。

**seq ↔ seq 通信**（如 WishboneROMAdapter → ROM，两者都是 seq）：

```
总延迟 = 1（适配器 seq 输出→_w[]） + ROM内部延迟 + 1（ROM seq 输出→_w[]→适配器读）
       = ROM内部延迟 + 2
```

例如 ROM 的 `read_latency=2`，适配器需要 `wait = 2 + 2 = 4` tick 从 stb 到 ack。

### 数据缓存与总线延迟（2026-11 实测结论）

`wb_dcache` 是独立的写直通 + snoop 数据缓存元件（16 B 行、按字有效、命中读在组合
阶段应答），可以串在 CPU 和互连之间。**在 riscv_linux.json 上实测没有收益**：
500M 拍级 A/B 落进噪声，启动反而慢约 5%。原因是 CPU 单次 load 固定 4 个周期，
延迟来自 CPU 自己的流水线，总线 ack 的那 1~2 拍早就藏在里面；缓存命中省不掉这
4 拍，未命中反而多一次组件级往返。以后想用它，先确认从设备是长延迟的（例如
ROM 的 `read_latency`），不要默认插进 CPU 通路。

互连（`wishbone_ic`）的主设备侧应答/读数据走**组合阶段**：从设备本拍写下的
ack/dat 下一拍就组合多路选择给主设备，从设备侧请求仍在时序阶段发出。这样经互连
访问和直连从设备的周期数相同（`tests/test_wishbone_ic/perf_latency.cpp` 守着这条
不变量）；空闲拍不重写主设备侧输出（`_wbic_any` 门控）。

## 电路 JSON 的两种文件形式

顶层电路和可复用的蓝图可以分开存文件，两者都带可选的 `$meta`：

```json
"$meta": { "kind": "circuit", "format": 1, "author": "...", "description": "..." }
```

- 没有 `$meta` 按 circuit 处理（向后兼容）；蓝图必须写 `kind: "blueprint"`，
  直接当顶层电路加载会报错。
- `format` 是 JSON 数据格式版本（`kJsonFormatVersion`，见 `CircuitSerializer.h`），
  加载时拒绝比程序新的版本。新增可选字段不用升版本。
- 顶层 `definitions` 的值可以是内联定义对象，也可以是蓝图文件路径。相对路径先按
  引用它的文件所在目录找，再按 `-I/--lib-path` 的顺序找
  （`CircuitLibrary::searchPaths()`），和 C++ 头文件搜索一致。
- 蓝图文件里可以写 `definitions` 映射（一个文件多个定义），也可以把单个定义直接
  写在根上，名字取 `$meta.name`，没写就用引用处的键。
- 文件引用先导入、顶层内联定义后导入，同名时以内联定义为准；蓝图之间循环引用
  会报错。`schemas/*.schema.json` 只给 IDE 提示，不参与校验。

排查工具：`dcs --check file.json`（电路或蓝图，加载 + 展开 + 静态检查，不编译）、
`dcs --list-definitions file.json`（定义名和来源文件）、`dcs --dump-schema out.json`
（从元件注册表生成 schema）。查找路径 = `-I/--lib-path`（可重复）在前、
`DCS_LIB_PATH`（PATH 风格）在后。

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

