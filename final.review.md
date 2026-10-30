# 审查范围与背景

这是一个 RISC-V 软核 + 数字电路仿真器的 C++ 实现。本轮改动集中在：

1. **RiscVCPU**（`src/components/RiscVCPU.cpp` / `include/dcs/components/RiscVCPU.h`）
   - MMU 从 Sv32 专用改成 Sv32/Sv39 通用的逐级遍历（`mmuTranslate` + `handlePte`），
     Sv39 的 PTE 8 字节拆成低/高两笔总线事务读，支持 2MB/1GB 大页和 canonical 地址检查
   - `takeTrap` 不再在 trap 入口清 `mstatus.MPRV`（规范只在 xRET 返回低于 M 的特权级时清）
   - Zfh 半精度浮点：half 用 double 承载中间值，`doubleToHalf` 手写按 rm 舍入
   - Debug 触发模块：tselect/tdata1-3/tinfo/tcontrol + mcontrol 地址匹配
2. **CodeGen/Component**：每拍传播改成定长 16 字节拷贝，输入/输出指针表改 static 初始化
3. **WishboneMemory**：地址按容量折算（高位地址线不存在时自然别名）
4. **UART**：改成 16550A 兼容寄存器布局
5. **ElfLoader/main**：按 EI_CLASS 自动解析 32/64 位 ELF，装载偏移按容量折算，PC 设到入口

# 审查重点

只报**真实的正确性缺陷**：会导致错误结果、崩溃、死锁、规范违背的问题。
优先级从高到低：

1. MMU：Sv39 三级遍历的 PTE 地址计算、大页对齐检查、A/D 处理、TLB 装填与 ASID、
   canonical 检查、跨 4GB 的访问错误判定是否自洽
2. Zfh：`doubleToHalf` 的舍入（RNE/RTZ/RDN/RUP/RMM）、非规格化/溢出边界、
   NaN-boxing 两个方向、无效运算 NV 标志
3. Debug 触发：命中条件、breakpoint 异常时机（指令不能产生副作用）、mepc 指向
4. 性能优化：改成 16 字节定长拷贝、static 指针表是否破坏了任何语义
   （例如读到上一拍残留值、多实例串扰）
5. UART：16550 寄存器语义（DLAB 切换、LSR/IIR 状态、读写副作用）
6. ELF 加载：地址折算、入口设置、tohost 判定

**不要报**：风格、命名、注释措辞、过度防御建议、不影响正确性的微优化。
如果确实没有真问题，直接说"未发现正确性问题"，不要凑数。

# 已知设计约束（不是 bug）

- A/D 缺失走 page fault 交给软件，不硬件改写页表（dirty.S 两条路径都接受）
- 物理地址总线只有 32 位，超过报访问错误
- 存储器地址位宽有限，高位地址按容量别名（等价于芯片上没接那么多地址线）
- 不做 P 扩展（官方已废弃）
- 软件（OpenSBI/内核）不在本工程范围内
