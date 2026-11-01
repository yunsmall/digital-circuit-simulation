#!/bin/bash
# 把内核 Image、设备树、initramfs 打包成 dcs 可直接加载的单个 ELF。
# 入口 stub @0x80000000，DTB @0x80100000，Image @0x80200000，initramfs @0x82000000。
# DTB 里的 linux,initrd-end 必须跟着 initramfs 实际大小走，否则内核解压范围不对。
#
# 用法: build-boot.sh <initramfs.cpio.gz> [输出 elf]
# 环境变量:
#   LINUX_SRC  内核源码树根目录（需已 make ARCH=riscv defconfig && make 完成）
#              默认 ~/code/linux/riscv64
#   CC / LD / OBJCOPY  工具链命令，默认自动探测
set -e
# 必须在任何 cd 之前算好，$0 是相对路径时后面再算就失效了
DCSDIR=$(cd "$(dirname "$0")" && pwd)
PROJ=$(cd "$DCSDIR/.." && pwd)

# 有些发行版只装了带版本后缀的可执行文件，所以按"通用名优先、版本后缀兜底"探测
pick_tool() {
    for c in "$@"; do
        if command -v "$c" > /dev/null 2>&1; then
            echo "$c"
            return 0
        fi
    done
    echo "缺少工具: $*" >&2
    return 1
}
CC=${CC:-$(pick_tool clang clang-23 clang-22 clang-21 clang-20 clang-19)}
LD=${LD:-$(pick_tool ld.lld ld.lld-23 ld.lld-22 ld.lld-21 ld.lld-20 ld.lld-19)}
OBJCOPY=${OBJCOPY:-$(pick_tool llvm-objcopy llvm-objcopy-23 llvm-objcopy-22 llvm-objcopy-21 llvm-objcopy-20)}

LINUX_SRC=${LINUX_SRC:-$HOME/code/linux/riscv64}
IMAGE=$LINUX_SRC/linux-6.12/arch/riscv/boot/Image

INITRD=${1:?用法: build-boot.sh <initramfs.cpio.gz> [输出 elf]}
OUT=${2:-$PROJ/images/boot.elf}

[ -f "$IMAGE" ] || { echo "找不到内核 Image: $IMAGE（先编译内核，或用 LINUX_SRC 指定源码树）" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# 按 initramfs 实际大小改写 linux,initrd-end，内核靠这个范围解压
size=$(stat -c%s "$INITRD")
end=$(printf '0x%x' $((0x82000000 + size)))
sed "s/linux,initrd-end = <0x0 0x[0-9a-fA-F]*>/linux,initrd-end = <0x0 ${end}>/" \
    "$DCSDIR/dcs-riscv.dts" > dcs.dts
dtc -I dts -O dtb -o dcs.dtb dcs.dts > /dev/null 2>&1

# objcopy 生成的 .o 没有浮点 ABI 标记，boot.o 必须用 lp64 而不是 lp64d，
# 否则 lld 会以 "different floating-point ABI" 拒绝链接
"$CC" --target=riscv64 -march=rv64gc -mabi=lp64 -c "$DCSDIR/boot.S" -o boot.o
"$OBJCOPY" -I binary -O elf64-littleriscv -B riscv \
    --rename-section .data=.dtb,alloc,load,readonly,contents dcs.dtb dtb.o
"$OBJCOPY" -I binary -O elf64-littleriscv -B riscv \
    --rename-section .data=.kernel,alloc,load,readonly,contents "$IMAGE" kernel.o
"$OBJCOPY" -I binary -O elf64-littleriscv -B riscv \
    --rename-section .data=.initrd,alloc,load,readonly,contents "$INITRD" initrd.o

mkdir -p "$(dirname "$OUT")"
"$LD" -T "$DCSDIR/boot.ld" -o "$OUT" boot.o dtb.o kernel.o initrd.o
ls -la "$OUT"
