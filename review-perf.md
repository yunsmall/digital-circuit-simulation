# 审查范围与背景

这是一个 RISC-V 软核 + 数字电路仿真器的 C++ 实现。本轮改动全部是为了提升仿真速度，
风险点集中在"为了更快而改变了时序/缓存/调用路径"，请只找**语义被改坏**的地方。

重点文件与改动：

1. **src/components/RiscVCPU.cpp — 取指与执行流水**
   - icache 命中时不再进入 EXECUTE 相位，而是当拍直接调用 `do_execute()`
   - 跨字（4n+2 起）32 位指令：命中时用相邻缓存行的原始字拼装；
     拼装完成后把结果回填到首字所在行（`inst[1]`、`len[1] = 4`）
   - M 态取指走快捷路径，不再调用 `mmuTranslate`
   - rst / clk 直接读 `inputs[]`，下降沿不再构造完整的 CpuIn 结构；
     中断线只在时钟上升沿采样
   - `cpuTick` 标记了 `no_stack_protector`
   - D 扩展的 FMUL/FDIV/FSQRT 余项计算从 `std::fma` 换成 Dekker 拆分（`twoProduct`）

2. **src/components/WishboneMemory.cpp — 纯 JIT 化**
   - 原本是 C++ 回调，现在访存协议直接生成进 JIT 代码里；
     状态放在 C 侧静态结构体，内存缓冲通过固定地址访问
   - 输出直接写 `_w[]`（不经 `genOutputWrite` 的高位清零）

3. **include/dcs/util/RvcDecode.h**
   - 新增 C.FLD / C.FSD / C.FLDSP / C.FSDSP 的解压（FSD 用独立 opcode 0x27）

4. **src/CodeGen.cpp / src/Component.cpp / include/dcs/Component.h**
   - 新增 `circuit_tick_cycle` / `circuit_tick_cycles`（JIT 内直接翻转时钟线网）
   - 新增 `genOutputWirePtrArray`：回调直接写 `_w[]`
   - `genOutputWrite` 增加 drive 参数（三态使能由它统一生成）

# 审查重点

只报**会导致错误结果、崩溃、或者与 RISC-V 规范/总线时序不符**的问题。优先级：

1. 取指流水：命中直接执行是否漏掉某个相位该做的事（中断采样、指令边界、trap 时机）；
   跨字指令拼装与回填是否可能读到错的行（tag 比较、4n+2 边界、跨 1KB 边界）
2. icache 一致性：自修改代码 / fence.i 之后缓存是否该失效（如果原来就没有，不算新问题，
   但如果本轮改动让它更严重就要报）
3. 下降沿不再读中断线：是否有场景依赖"下降沿看到中断"（如 WFI 唤醒、mip 更新时机）
4. WishboneMemory 纯 JIT：状态清零、地址折算、AMO 读后写、ack 撤销时机是否与原来等价
5. RVC 解压：C.FLD/C.FSD/C.FLDSP/C.FSDSP 的位段是否正确（对照 RISC-V 规范）
6. Dekker 拆分替代 fma：在溢出/次正规/零附近的正确性，以及 NX/UF 标志

**不要报**：风格、命名、注释措辞、"建议加防御性检查"、不影响正确性的微优化。

如果确实没有真问题，直接说"未发现正确性问题"，不要凑数。

# 已知设计约束（不是 bug）

- 物理地址总线只有 32 位，超过报访问错误
- 存储器地址位宽有限，高位地址按容量别名
- A/D 缺失走 page fault 交给软件
- 不做 P 扩展；软件（OpenSBI/内核）不在本工程范围内
- 单核、顺序执行、无多发射，目标只是"行为等价"而不是周期精确
