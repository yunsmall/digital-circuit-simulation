# 在 dcs 上启动 Linux

这个目录让 dcs 的 RISC-V CPU 模型真正跑起一个 Linux 6.12 用户空间。

整个内核镜像、设备树、initramfs 会被打包成**一个 ELF**（`images/boot.elf`），
dcs 用 `--elf` 直接加载，不需要外部 bootloader：

```
0x80000000  启动 stub（boot.S：设置 a0/a1、委派异常、mret 进 S 态）
0x80100000  设备树 DTB
0x80200000  内核 Image（按 0x80200000 链接）
0x82000000  initramfs
```

## 依赖

- Linux 或 WSL2（Ubuntu 24.04 验证过）
- Clang（含 `ld.lld`、`llvm-objcopy`）——脚本会自动探测，优先用无版本后缀的通用名
- `dtc`（设备树编译）、`cpio`、`gzip`、`gcc`
- 内核源码 6.12（脚本会下载或复用）

装的命令（Ubuntu）：

```bash
sudo apt-get install -y clang lld llvm device-tree-compiler cpio gzip
# 若发行版只有带版本后缀的包（如 clang-22/clang-23），装了即可，脚本会自己找到
# 编 busybox 还要这两个：musl 的静态库从源码编，riscv64 的 crt/libgcc 来自交叉包，
# clang 通过 --gcc-install-dir 直接使用它（不需要整条 GCC 交叉工具链）
sudo apt-get install -y libgcc-14-dev-riscv64-cross
```

## 快速开始

```bash
# 1. 下载并编译内核（约 20~30 分钟，只需一次）
export LINUX_SRC=~/code/linux/riscv64
mkdir -p "$LINUX_SRC" && cd "$LINUX_SRC"
curl -LO https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz
tar xf linux-6.12.tar.xz
cd linux-6.12
make ARCH=riscv defconfig
make ARCH=riscv -j6 Image          # 只需要 Image，不用 modules

# 2. 做一个最小的 initramfs（只含 /init，不依赖任何库）
cd "$LINUX_SRC"
clang --target=riscv64 -march=rv64gc -mabi=lp64d -fuse-ld=lld \
    -static -nostdlib -O2 \
    <本目录>/dcs-init.S -o dcs-init
gcc -O2 -o gen_init_cpio linux-6.12/usr/gen_init_cpio.c
printf 'dir /dev 755 0 0\nnod /dev/console 644 0 0 c 5 1\nnod /dev/null 666 0 0 c 1 3\nfile /init %s/dcs-init 755 0 0\n' "$LINUX_SRC" > init.spec
./gen_init_cpio init.spec | gzip -9 > initrd.cpio.gz

# 3. 打包成一个 ELF
bash <本目录>/build-boot.sh initrd.cpio.gz

# 4. 跑
cd <项目根>
./build/RelWithDebInfo/dcs_soc examples/jsons/riscv_linux.json --elf images/boot.elf --forever
```

`dcs-init.S` 会打开 `/dev/console` 写一句横幅再 `exit`，看到

```
[dcs] Hello from Linux userspace!
```

就说明用户态、串口、tty、系统调用全部打通了。

## 带 busybox 的可交互 shell

`build-busybox.sh` 用 clang 交叉编译一个静态 busybox 并做成 initramfs
（不需要 GCC 交叉工具链，也不需要 sudo）：

```bash
# 脚本从 $LINUX_SRC/src/musl-1.2.5.tar.gz 编 musl 静态库，第一次跑之前放好
mkdir -p "$LINUX_SRC/src" && cd "$LINUX_SRC/src"
curl -LO https://musl.libc.org/releases/musl-1.2.5.tar.gz

bash <本目录>/build-busybox.sh        # 产物 /tmp/initrd-busybox.cpio.gz
bash <本目录>/build-boot.sh /tmp/initrd-busybox.cpio.gz
./build/RelWithDebInfo/dcs_soc examples/jsons/riscv_linux.json --elf images/boot.elf --forever
```

进到 `~ #` 提示符就能敲命令了。

### 一次跑一批命令（不用守着交互）

改硬件模型后每次都要重启内核验证，逐条敲命令太慢。`--feed` 可以在启动完成后
自动把一段脚本喂给串口：

```bash
cat > /tmp/cmds.txt <<'EOF'
ls /
cat /proc/tty/drivers
uname -a
EOF
./build/RelWithDebInfo/dcs_soc examples/jsons/riscv_linux.json --elf images/boot.elf \
    -t 4000000000 --feed /tmp/cmds.txt --feed-after 3500000000
```

`--feed-after` 是到第几拍开始喂。要等内核把串口驱动注册好（约 35 亿拍）再发，
早了字节会丢在硬件 FIFO 里。

## 文件说明

| 文件 | 作用 |
|------|------|
| `boot.S` | 启动 stub：a0=hartid、a1=DTB 地址、委派异常/中断、mret 落入 S 态 |
| `boot.ld` | 四段分散布局的链接脚本 |
| `dcs-riscv.dts` | 设备树：内存、CLINT、PLIC、16550 UART |
| `dcs-init.S` | 最小 init（`open("/dev/console")` + `write` + `exit`） |
| `dcs-init.c` | 同上，用内核自带 nolibc 的 C 版本 |
| `build-boot.sh` | 把 Image/DTB/initramfs 打包成单个 ELF |
| `build-busybox.sh` | 交叉编译 musl 静态 busybox 并打 initramfs |
| `make-disk.sh` | 生成 virtio-blk 用的空磁盘镜像 |

## virtio 设备

`riscv_linux.json` 里接了四台 virtio-mmio 设备，全部是**总线主设备**：virtqueue 的
descriptor/avail/used 和数据缓冲都通过 `wishbone_ic` 的主端口访问，不直接碰存储器内部。

| 设备 | MMIO | PLIC 源 | 主端口 | 宿主侧对接 |
|------|------|---------|--------|-----------|
| `virtio_blk` | `0x10001000` | 2 | m1 | `images/disk.img`，也可换成 ROM 或内置存储 |
| `virtio_net` | `0x10002000` | 3 | m2 | slirp 用户态网络（JSON 里 `"backend": "slirp"`） |
| `virtio_rng` | `0x10003000` | 4 | m3 | JIT 内生成随机数 |
| `virtio_input` | `0x10004000` | 5 | m4 | 宿主 `pushInputEvent` 塞事件、`popStatusEvent` 取 LED 状态 |

```bash
bash <本目录>/make-disk.sh                 # 16 MB 空镜像
# 可选：在宿主上格式化
mke2fs -t ext4 -F images/disk.img

./build_clang/dcs_soc examples/jsons/riscv_linux.json --elf images/boot.elf --forever --watch tmp/cmds.txt
# guest 里
ls -l /dev/vda /dev/hwrng
ip link set eth0 up && udhcpc -i eth0
ip addr add 10.0.2.15/24 dev eth0     # udhcpc 的默认脚本在精简 rootfs 里不一定配地址
ip route add default via 10.0.2.2
ping -c 3 10.0.2.2
ping -c 3 8.8.8.8
```

### 宿主机连进 guest（端口转发 + telnetd）

slirp 后端支持 qemu 风格的主机端口转发，写在 `virtio_net` 的 `backend_opts`
（嵌套对象，加载时拍平成 `backend_opts.hostfwd` 参数键）里：

```json
"params": {
  "backend": "slirp",
  "backend_opts": { "hostfwd": "tcp:127.0.0.1:2223-10.0.2.15:23" }
}
```

`hostfwd` 的格式是 `[tcp|udp:]宿主地址:端口-客机地址:端口`，多条用 `;` 分隔，地址
省略时默认 127.0.0.1 / 10.0.2.15。busybox 的 init 在 `ifconfig eth0` 成功后启动
`telnetd -p 23`（依赖 devpts），宿主机直接连：

```bash
telnet 127.0.0.1 2223
```

就能拿到 guest 的 shell。这是 slirp 后端专有的参数，换别的后端不会有。

编 slirp 后端默认是关的（CI 也不编它）：

```bash
vcpkg install libslirp
cmake -B build -DDCS_BUILD_NET=ON ...   # 其余参数同项目根目录 README
```

元件本身不解释 `backend_opts` 里的键，后端按前缀取自己那段（见
`include/dcs/net/BackendOpts.h`）。

实测结果（2026-11-01）：`udhcpc` 拿到 10.0.2.15，ping 网关和 8.8.8.8 都是 0% 丢包；
`/dev/vda` 可读、`/dev/hwrng` 每次读出新的随机字节；`virtio_input` 以
`dcs-virtual-input` 注册为键盘（`/proc/bus/input/devices` 可见）。
当前内核配置没有打开 `CONFIG_INPUT_EVDEV`，所以用户态拿不到 `/dev/input/event*`，
宿主侧事件要通过 `VirtioInput::pushInputEvent()` 从程序里喂。

`QueueNumMax` 报的是 256：Linux 的 virtio-net 在环空位不足 `2+MAX_SKB_FRAGS`
（约 19）时会直接停掉 TX 队列，环给太小会表现为"发一个包之后就再也不发"。

宿主的每次读写都直接落到镜像文件上，所以 guest 里写的数据在仿真退出后仍然保留。
镜像也可以来自电路里的 ROM（把 `path` 留空），这时设备声明 `VIRTIO_BLK_F_RO`，
写请求返回只读错误。

调试时加 `--trace-cpu`：连续 400 万拍 PC 不变就打印 `PC/halted/ra/sp/a0`
（配合内核源码树的 `System.map` 可以还原符号）。设备主端口或互连长时间不推进时，
还会在 stderr 打 `[dcs] virtio#...` / `[dcs] wishbone 卡住 ...`；virtio 的常规寄存器
追踪默认关闭，排查队列问题时设 `DCS_VIRTIO_TRACE=1` 再打开。

## 几个容易踩的坑

- **busybox 的 applet 要建链接**：`make defconfig` 编出的 busybox 带 400 多个
  applet，但 initramfs 里如果只手工建几个软链接，用户只能看到 `ls/cat/echo` 那几个。
  init 脚本里跑一次 `/bin/busybox --install -s /bin` 就全部可用（`vi`、`top`、
  `grep`、`ip`、`ping`、`wget`、`tar`、`awk`……）。
- **用 clang 编 busybox 必须 `-DBB_GLOBAL_CONST=`**：clang 会把 `BB_GLOBAL_CONST`
  （`const` 指针）当成不可变对象，把 `SET_PTR_TO_GLOBALS` 之后的读写都基于旧指针
  （初始为 NULL）做，`awk` 一启动就写 0x4 段错误。`build-busybox.sh` 里已经通过
  `CONFIG_EXTRA_CFLAGS` 关掉这个优化。
- **`reg-shift = <2>`**：UART 元件按 4 字节步进映射寄存器，设备树必须写 `reg-shift=2`，
  否则内核按字节偏移访问，读到的 LSR/IIR 全是错位数据。
- **PLIC 的 enable 是 32 位位图寄存器**：`0x2000 + context*0x80 + word*4`，
  位 `N%32` 表示源 N。写成"每个源一个寄存器"会导致内核写的使能全部落空，
  表现为外部中断永远送不到内核、tty 的 TX 卡在缓冲区里。
- **PLIC 要接 CPU 的 `seip`（S 态外部中断）**，不是 `irq`（M 态 MEI）——内核在 S 态等的是 SEI。
- **CLINT 的 mtimecmp 复位值**：没有 M 态固件初始化时留 0 会让 `mtime >= mtimecmp`
  从第一拍就恒真，内核一开定时器中断就是中断风暴。本项目的 CLINT 复位成 `~0`。
- **`keep_bootcon`**：bootargs 里带它时 earlycon 和正式 console 会各打印一遍，
  看到每行重复两遍是正常的；不需要时去掉即可。
- **initramfs 里要有 `/dev/console`**：否则内核打不开初始控制台，
  init 拿到的 fd 1 是 `/dev/null`，`write` 返回成功但什么也看不到。

## 已知限制

- 单 hart、单 context（内核只用 S 态 context 1）。
- UART 是单字节收发寄存器，不实现 FIFO；因此 IIR 不报告 FIFO 使能位。
- 没有 M 态固件（OpenSBI 的角色由 `boot.S` + CPU 内建的 SBI 调用处理）。
- virtqueue 必须落在 RAM；块设备镜像可以来自只读 ROM（设备会声明 `VIRTIO_BLK_F_RO`）。
