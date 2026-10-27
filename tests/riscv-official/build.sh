#!/bin/sh
# 用 clang 交叉编译官方 riscv-tests 用例，产物给 tests/test_riscv/official.cpp 加载。
# 源目录默认取仓库里的 tmp/riscv-tests-master（需含 riscv-test-env 的 env/p）。
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${RISCV_TESTS_DIR:-$ROOT/tmp/riscv-tests-master}
OUT=$ROOT/tests/riscv-official/out
CLANG=${CLANG:-clang}
mkdir -p "$OUT"

if [ ! -d "$SRC/env/p" ]; then
    echo "缺少 $SRC/env/p（riscv-test-env 子模块内容）" >&2
    exit 1
fi

ok=0; failed=0
for suite in rv32ui rv32um rv32ua; do
    case $suite in
        rv32ui) march=rv32i ;;
        *)      march=rv32im ;;
    esac
    for s in "$SRC/isa/$suite"/*.S; do
        name=$(basename "$s" .S)
        if "$CLANG" --target=riscv32-unknown-elf -march=$march -mabi=ilp32 \
             -nostdlib -nostartfiles -static \
             -I"$SRC/env/p" -I"$SRC/isa" -I"$SRC/isa/macros/scalar" \
             -T"$ROOT/tests/riscv-official/dcs_link.ld" \
             -Wl,--image-base=0x0 -Wl,--no-relax \
             -o "$OUT/$suite-p-$name.elf" "$s" 2>/dev/null; then
            ok=$((ok+1))
        else
            failed=$((failed+1))
        fi
    done
done
echo "编译完成: $ok 个, 失败 $failed 个 -> $OUT"
