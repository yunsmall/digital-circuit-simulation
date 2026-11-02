#!/bin/bash
# 用 clang 交叉编译官方 riscv-tests 用例（RV32 + RV64），产物给
# tests/test_riscv/official.cpp 与 tests/test_riscv64/official64.cpp 加载。
#
# 关于源码：官方源码在 .weak 里声明 mtvec_handler/stvec_handler，同时又在
# 测试里用 .global 把它们升为全局。clang 的集成汇编器不接受这种"弱符号升全局"，
# gcc 可以。本机只有 clang，所以这里把源码复制到临时目录并去掉这两行 .global，
# 仓库里不必再维护一份改过的副本——从 GitHub 下载的原始源码可以直接编译。
#
# 链接器必须用 lld（-fuse-ld=lld）：宿主是 x86 时系统自带的 ld 只有 x86 模拟
# （报 "unrecognised emulation mode: elf64lriscv"），只有 lld 能链接 riscv 目标。
#
# 用法: bash build.sh [输出目录32] [输出目录64]
#       环境变量 RISCV_TESTS_DIR 指定原始源码根（含 isa/ 与 env/），
#       默认依次找 tmp/riscv-tests-master、tests/riscv-official/src
#       环境变量 CLANG 指定编译器（默认 clang），JOBS 指定并行度（默认 6）
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CLANG=${CLANG:-clang}
JOBS=${JOBS:-6}
# 有些发行版只装 python3，没有 python 这个名字
PYTHON=${PYTHON:-$(command -v python || command -v python3)}
OUT32=${1:-$ROOT/tests/riscv-official/out}
OUT64=${2:-$ROOT/tests/riscv-official/out64}

if [ -n "$RISCV_TESTS_DIR" ] && [ -d "$RISCV_TESTS_DIR/isa" ]; then
    ISADIR=$RISCV_TESTS_DIR/isa
    ENVINC=$RISCV_TESTS_DIR/env
elif [ -d "$ROOT/tmp/riscv-tests-master/isa" ]; then
    ISADIR=$ROOT/tmp/riscv-tests-master/isa
    ENVINC=$ROOT/tmp/riscv-tests-master/env
else
    ISADIR=$ROOT/tests/riscv-official/src
    ENVINC=$ROOT/tests/riscv-official/env
fi

STRIP=$ROOT/tests/riscv-official/.build-src
LINK_LOW=$ROOT/tests/riscv-official/dcs_link.ld        # 链接到 0，映像直接装进 64KB 内存
LINK_DRAM=$ROOT/tests/riscv-official/dcs_link_dram.ld  # 链接到 DRAM_BASE，给用 DRAM_BASE 算地址的用例

mkdir -p "$OUT32" "$OUT64" "$STRIP"
CMDS=$(mktemp)
: > "$CMDS"

plan_suite() {
    xlen=$1; suite=$2; march=$3; mabi=$4; outdir=$5
    [ -d "$ISADIR/$suite" ] || return 0
    for s in "$ISADIR/$suite"/*.S; do
        [ -e "$s" ] || continue
        name=$(basename "$s" .S)
        mkdir -p "$STRIP/$suite"
        # 两阶段编译：先 C 预处理展开宏——.float16 藏在 test_macros.h 的宏体内，
        # 直接 grep 源文件看不到；展开后再把 clang 汇编器不认识的东西换掉
        $CLANG --target=riscv${xlen}-unknown-elf -march=$march -mabi=$mabi \
            -I"$ENVINC/p" -I"$ISADIR" -I"$ISADIR/macros/scalar" -I"$ENVINC" \
            -E "$s" -o "$STRIP/$suite/$name.i.S"
        # 去掉 .global mtvec_handler / .global stvec_handler（见文件头说明），
        # 并把 .float16 换成 .hword 位模式
        grep -v -E '^[[:space:]]*\.global[[:space:]]+(mtvec|stvec)_handler' "$STRIP/$suite/$name.i.S" \
            | "$PYTHON" "$ROOT/tests/riscv-official/fix_float16.py" > "$STRIP/$suite/$name.S"
        # dirty.S 用 "符号 - DRAM_BASE" 计算虚拟地址：只有链接在 DRAM_BASE 时
        # 这个差值才是它期望的低地址（链接在 0 会让 32 位减法回绕）
        link=$LINK_LOW
        [ "$name" = "dirty" ] && link=$LINK_DRAM
        echo "$CLANG --target=riscv${xlen}-unknown-elf -march=$march -mabi=$mabi -nostdlib -nostartfiles -static -fuse-ld=lld -x assembler -T$link -Wl,--no-relax -o $outdir/$suite-p-$name.elf $STRIP/$suite/$name.S || echo FAIL:$suite-p-$name.elf >> $CMDS.fail" >> "$CMDS"
    done
}

# mi/si 必须用带 F/D 的 march：它们的 csr.S 在 FS=Off 时走 #error 分支，
# 编成不带浮点的镜像会注定失败
for x in 32 64; do
    if [ "$x" = 32 ]; then
        mbase=rv32; abi=ilp32; abid=ilp32d; out=$OUT32
    else
        mbase=rv64; abi=lp64; abid=lp64d; out=$OUT64
    fi
    plan_suite $x rv${x}ui  ${mbase}i            $abi  $out
    plan_suite $x rv${x}um  ${mbase}im           $abi  $out
    plan_suite $x rv${x}ua  ${mbase}ima          $abi  $out
    plan_suite $x rv${x}uc  ${mbase}ic           $abi  $out
    plan_suite $x rv${x}uf  ${mbase}imaf         $abi  $out
    plan_suite $x rv${x}mi  ${mbase}id           $abid $out
    plan_suite $x rv${x}si  ${mbase}id           $abid $out
    plan_suite $x rv${x}uzba    ${mbase}im_zba    $abi $out
    plan_suite $x rv${x}uzbb    ${mbase}im_zbb    $abi $out
    plan_suite $x rv${x}uzbc    ${mbase}im_zbc    $abi $out
    plan_suite $x rv${x}uzbkb   ${mbase}im_zbkb   $abi $out
    plan_suite $x rv${x}uzbs    ${mbase}im_zbs    $abi $out
    plan_suite $x rv${x}uzfh    ${mbase}imafd_zfh $abi $out  # Zfh 依赖 F，fcvt.h.d 还要 D
    plan_suite $x rv${x}uzicond ${mbase}im_zicond $abi $out
done

TOTAL=$(wc -l < "$CMDS")
# 并行编译：每个进程一条 clang 命令
xargs -d '\n' -P "$JOBS" -I CMD sh -c CMD < "$CMDS"
FAILED=0
[ -f "$CMDS.fail" ] && FAILED=$(wc -l < "$CMDS.fail")
echo "编译完成: $((TOTAL-FAILED)) 个, 失败 $FAILED 个 -> $OUT32 / $OUT64"
rm -f "$CMDS" "$CMDS.fail"
