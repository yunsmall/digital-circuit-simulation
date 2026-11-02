"""把 32 位目标装不下的 `li` 立即数截断到 32 位。

riscv-tests 的 rv32uzbb/zext_h、sext_h、sext_b 是直接 include 对应的 rv64 源码，
里面的输入常量写成了完整 64 位（如 0x0000000f0007ffff）。RV32 的 `li` 只能装 32 位，
clang 会报 "immediate must be an integer in the range [-2147483648, 4294967295]"。

取低 32 位是安全的：这几条指令只看低 8/16 位，而且 TEST_R_OP 的期望值本来就按
XLEN 掩码过，截断后的结果和 64 位输入一致。只在 xlen=32 的构建里跑。
"""

import re
import sys

# li <寄存器>, 0x<超过 8 位的十六进制数>，取低 8 位十六进制
LI_IMM = re.compile(r"(\bli\s+[A-Za-z0-9_]+,\s*)0x([0-9a-fA-F]{9,})\b")


def _truncate(m: re.Match) -> str:
    return f"{m.group(1)}0x{m.group(2)[-8:]}"


sys.stdout.write(LI_IMM.sub(_truncate, sys.stdin.read()))
