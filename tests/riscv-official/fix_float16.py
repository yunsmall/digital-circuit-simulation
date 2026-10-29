#!/usr/bin/env python3
"""把 GNU as 的 .float16 伪指令转成 .hword 位模式。

clang 的集成汇编器不认识 .float16（Zfh 测试源码和 test_macros.h 里大量使用），
而 .hword 是两种汇编器都支持的通用指令。Python 的 struct 'e' 就是
IEEE 754 binary16，舍入规则与汇编器的 .float16 一致（RNE）。

支持的参数形式：
  普通浮点字面量（含科学计数法）、NaN、Inf/Infinity、以及 0h:xxxx 位模式字面量。

用法: python fix_float16.py < 输入.S > 输出.S
"""
import re
import struct
import sys

# 浮点字面量 / NaN / Inf / 0h:hex 位模式
PAT = re.compile(
    r"\.float16\s+("
    r"[-+]?0h:[0-9a-fA-F]+"                     # LLVM 位模式字面量
    r"|[-+]?(?:nan|NaN|inf|Inf|Infinity)"       # 特殊值
    r"|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"  # 普通浮点
    r")"
)


def repl(m):
    tok = m.group(1)
    if "h:" in tok.lower():
        neg = tok.startswith("-")
        bits = int(tok.split(":", 1)[1], 16) & 0xFFFF
        if neg:
            bits |= 0x8000
        return ".hword 0x%04x" % bits
    low = tok.lower().lstrip("+-")
    if low.startswith("nan"):
        bits = 0x7E00
        if tok.startswith("-"):
            bits |= 0x8000
        return ".hword 0x%04x" % bits
    if low.startswith("inf"):
        bits = 0x7C00
        if tok.startswith("-"):
            bits |= 0x8000
        return ".hword 0x%04x" % bits
    v = float(tok)
    bits = struct.unpack("<H", struct.pack("<e", v))[0]
    return ".hword 0x%04x" % bits


def main():
    text = sys.stdin.read()
    sys.stdout.write(PAT.sub(repl, text))


if __name__ == "__main__":
    main()
