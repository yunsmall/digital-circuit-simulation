#!/bin/bash
# 把 benchmarks/bench.c 的各个场景编译成 RV64 ELF，产物给 benchmarks/benchmark.cpp 使用。
# 用法: bash benchmarks/build.sh [输出目录]
#       CLANG 环境变量可指定编译器（默认 clang）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HERE="$ROOT/benchmarks"
CLANG=${CLANG:-clang}
OUT=${1:-$HERE/out}

mkdir -p "$OUT"

# 场景编号必须与 bench.c 里的 #if BENCH 对应
NAMES=(alu mem branch muldiv fp mixed)

for i in "${!NAMES[@]}"; do
    name=${NAMES[$i]}
    "$CLANG" --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d \
        -nostdlib -nostartfiles -static -O2 -DBENCH=$i \
        -T "$HERE/bench.ld" -Wl,--no-relax \
        -o "$OUT/$name.elf" "$HERE/start.S" "$HERE/bench.c"
    echo "built $OUT/$name.elf"
done
