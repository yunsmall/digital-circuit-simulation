# examples — 可运行电路

`examples/` 只放**顶层电路**：给蓝图接上常量、时钟、显示件，直接能跑。
可复用的功能块在项目根的 `library/`，两者分开，避免把“积木”和“成品”混在一起。

## 目录

| 目录 | 内容 |
|------|------|
| `combinational/` | 组合逻辑：半加器、4 位加法器、参数化加法器、ALU、mux、译码器、优先编码器、比较器、奇偶校验、格雷码、三态总线 |
| `sequential/` | 时序逻辑：D 触发器、移位寄存器、环形计数器、BCD 计数显示、加/减计数、分频、去抖、同步器、浮点流水线 |
| `fsm/` | 状态机：交通灯 |
| `memory/` | RAM 读写、FIFO 回环、寄存器堆 |
| `platform/` | GPIO 读输入、RTC 走时 |
| `soc/` | `riscv.json`（裸核）、`riscv_linux.json`（Linux 整机，需要 `--disk`） |

## 跑一个示例

```bash
./build_tcc/dcs.exe examples/combinational/adder_param.json -t 20 -m
```

`-m` 逐拍打印线网值；带 `display` 元件的电路在 GUI/宿主里能直接看到指示灯和数码管。

整机：

```bash
./build_tcc/dcs_soc.exe examples/soc/riscv_linux.json \
  --elf images/boot.elf --disk images/disk.img --forever
```

磁盘镜像走 `--disk`，不写死在命令行里；`riscv_linux.json` 自己也用 `file` 后端指向
`../../images/disk.img`（JSON 相对路径），所以不传 `--disk` 也能从 busybox 启动，
传了则换成另一块盘。`virtio_blk` 的组件默认后端是 `mem`（16MB 空白内存盘），
普通电路不提供镜像也能跑。

## 蓝图和参数

`library/` 里是可实例化的蓝图。引用方式：

```json
"definitions": { "adder": "../../library/adder.json" },
"components": [{
  "type": "composite",
  "name": "u_add",
  "params": { "definition": "adder", "bit_width": "16" },
  "connections": { "a": "cnt", "b": "one", "cin": "zero", "sum": "sum", "cout": "cout" }
}]
```

蓝图定义里可以写：

```json
"params": { "bit_width": "8" },
"components": [{ "type": "adder", "params": { "bit_width": "${bit_width}" } }],
"expose": { "inputs": { "a": { "net": "a", "width": "${bit_width}" } } }
```

规则：

- `params` 给默认值，实例同名参数覆盖；
- `${...}` 在配方参数、实例名、线网名、端口位宽里求值，缺参数直接报错；
- 里面可以写整数四则运算和括号：`${bit_width+1}`、`${(w+1)*2}`；`+` 两边不都是整数时
  按字符串拼接（`${prefix+suffix}`）；`- * /` 只接受整数，除零/溢出报错；
- 端口写 `{ "net": "a", "width": "${bit_width}" }` 时按求值结果定位宽，1~128；
- 求值结果不做范围钳制；没有字符串字面量，固定文本写在 `${}` 外面；
- 没有循环展开。端口数量随位宽变化的结构（逐位 a0..aN）要么用总线端口，
  要么直接用内置的 `adder`/`register` 这类带 `bit_width` 的元件。
