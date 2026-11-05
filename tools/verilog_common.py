#!/usr/bin/env python3
"""verilog2dcs.py / verilog2dll.py 共用的 Verilator 调用与 AST 解析。

两个脚本只是在"生成什么"上不同（dcs 元件源码 vs 外部 DLL），
verilator 命令行、端口/位宽/时序的提取规则必须完全一致，否则同一个 .v
在两条路上会得到不同的引脚。公共部分都放这里。
"""
import json
import os
import pathlib
import re
import subprocess
import sys

VERILATOR_BIN = os.environ.get("VERILATOR_BIN", "E:/msys64/ucrt64/local/bin/verilator_bin.exe")
VERILATOR_ROOT = os.environ.get("VERILATOR_ROOT", "E:/msys64/ucrt64/local/share/verilator")
VERILATOR_INCLUDE = os.environ.get(
    "VERILATOR_INCLUDE", str(pathlib.Path(VERILATOR_ROOT) / "include"))

DIR_MAP = {"INPUT": "input", "OUTPUT": "output", "INOUT": "inout"}


def run_verilator(args, workdir: pathlib.Path, *, verilator_bin=None, verilator_root=None) -> None:
    workdir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["VERILATOR_ROOT"] = verilator_root or VERILATOR_ROOT
    env.setdefault("LC_ALL", "C")
    r = subprocess.run([verilator_bin or VERILATOR_BIN, *map(str, args)], env=env,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("verilator 失败")


def verilate(src: pathlib.Path, top: str, workdir: pathlib.Path, *, verilator_bin=None,
             verilator_root=None):
    """生成模型源码，并额外导出一份 AST（端口/时序信息都从 AST 读）。"""
    # --pins-inout-enables：顶层 inout 会被拆成 input + <name>__en + <name>__out，
    # 这是 Verilator 官方支持的双向端口建模方式，正好对应 dcs 的三态总线
    common = ["--cc", "--pins-inout-enables", "--top-module", top, src]
    run_verilator([*common, "--Mdir", workdir], workdir,
                  verilator_bin=verilator_bin, verilator_root=verilator_root)
    header = workdir / f"V{top}.h"
    if not header.exists():
        raise SystemExit(f"没有生成 V{top}.h（--top 是否和模块名一致？）")

    # --json-only-output 本身不产出 C++，所以要和上一步分开跑
    ast = workdir / "ast.json"
    run_verilator([*common, "--json-only-output", ast, "--Mdir", workdir / "ast"], workdir,
                  verilator_bin=verilator_bin, verilator_root=verilator_root)
    return ast, header


def const_int(node):
    """CONST 节点 dump 出来的名字形如 "32'sh1"，取出数值。"""
    if isinstance(node, list):
        node = node[0] if node else None
    if not isinstance(node, dict) or node.get("type") != "CONST":
        return None
    m = re.fullmatch(r"\s*\d*'[sS]?([hHdDbBoO])([0-9a-fA-F]+)\s*", node.get("name", ""))
    if not m:
        return None
    return int(m.group(2), {"h": 16, "d": 10, "b": 2, "o": 8}[m.group(1).lower()])


def type_width(types: dict, dtype_addr: str, port_name: str) -> int:
    t = types.get(dtype_addr)
    if t is None:
        return 1
    kind = t.get("type")

    # packed 多维端口在 AST 里是嵌套的 PACKARRAYDTYPE，总位宽是各维长度之积
    if kind == "PACKARRAYDTYPE":
        elem = type_width(types, t.get("refDTypep"), port_name)
        total = elem
        for rng in as_list(t.get("rangep")):
            lo, hi = const_int(rng.get("leftp")), const_int(rng.get("rightp"))
            if lo is None or hi is None:
                raise SystemExit(f"端口 {port_name} 的维度不是常量，暂不支持")
            total *= abs(lo - hi) + 1
        return total

    # 存储器端口没有 range；当成 1 位会静默生成错误元件
    if kind == "UNPACKARRAYDTYPE":
        raise SystemExit(f"端口 {port_name} 是数组端口（unpacked array），"
                         f"dcs 线网对应不上：请拆成多个标量端口")

    rng = t.get("range")
    if not rng:
        if kind == "BASICDTYPE":
            return 1
        raise SystemExit(f"端口 {port_name} 的类型 {kind} 暂不支持")
    m = re.fullmatch(r"\s*(-?\d+)\s*:\s*(-?\d+)\s*", str(rng))
    if not m:
        raise SystemExit(f"端口 {port_name} 的位宽不是常量区间（{rng}），参数化 range 暂不支持")
    return abs(int(m.group(1)) - int(m.group(2))) + 1


def as_list(v):
    return v if isinstance(v, list) else []


def expand_port(types: dict, node):
    """把一个端口展开成若干标量引脚：unpacked 数组按维展开成 name_0、name_1……

    dcs 的线网是单值线网，数组端口只能拆成独立引脚。元素本来就是独立信号，
    拆开没有语义损失。
    """
    out = []
    base_name = node.get("verilogName") or node.get("name")
    base_cpp = node.get("name")

    def walk(addr, pin_name, cpp_expr):
        t = types.get(addr)
        if t is None:
            out.append({"name": pin_name, "cpp": cpp_expr, "width": 1})
            return
        if t.get("type") == "UNPACKARRAYDTYPE":
            ranges = as_list(t.get("rangep"))
            if not ranges:
                raise SystemExit(f"端口 {pin_name} 的数组维度无法解析")

            def walk_dims(idx, name, cpp):
                if idx == len(ranges):
                    walk(t.get("refDTypep"), name, cpp)
                    return
                lo = const_int(ranges[idx].get("leftp"))
                hi = const_int(ranges[idx].get("rightp"))
                if lo is None or hi is None:
                    raise SystemExit(f"端口 {pin_name} 的数组维度不是常量，暂不支持")
                step = 1 if hi >= lo else -1
                for i in range(abs(hi - lo) + 1):
                    # cpp 索引用 Verilog 里的真实下标（降序声明时是 3、2、1、0）
                    v = lo + i * step
                    walk_dims(idx + 1, f"{name}_{v}", f"{cpp}[{v}]")

            walk_dims(0, pin_name, cpp_expr)
            return
        out.append({"name": pin_name, "cpp": cpp_expr,
                    "width": type_width(types, addr, pin_name)})

    walk(node.get("dtypep", ""), base_name, base_cpp)
    return out


def parse_design(ast_path: pathlib.Path, top: str):
    """从 Verilator AST 里取出顶层模块的端口和有沿触发的 always 块。"""
    data = json.loads(ast_path.read_text(encoding="utf-8"))
    mod = next((m for m in as_list(data.get("modulesp"))
                if m.get("verilogName") == top or m.get("name") == top), None)
    if mod is None:
        raise SystemExit(f"AST 里找不到顶层模块 {top}")

    types = {}
    for misc in as_list(data.get("miscsp")):
        for t in as_list(misc.get("typesp")):
            if t.get("addr"):
                types[t["addr"]] = t

    ports = []
    for node in mod.get("stmtsp", []):
        if node.get("type") != "VAR" or not node.get("isPrimaryIO"):
            continue
        direction = DIR_MAP.get(node.get("direction", ""), "unknown")
        for p in expand_port(types, node):
            p["dir"] = direction
            ports.append(p)
    if not ports:
        raise SystemExit("顶层模块没有端口")

    # inout 端口靠 Verilator 的 --pins-inout-enables 拆成三件套：
    #   <name>      input    外部驱动进来的值
    #   <name>__out output   本模块驱动的值
    #   <name>__en  output   按位的驱动使能
    # dcs 侧对应：输入引脚 <name>_i 接总线，三态输出引脚 <name> 接同一条总线，
    # 阶段 2b 会在两者之间迭代到不动点，和真实硬件的收敛过程一致
    expanded = []
    for p in ports:
        if p["dir"] != "inout":
            expanded.append(p)
            continue
        if "[" in p["cpp"]:
            raise SystemExit(f"inout 数组端口 {p['name']} 暂不支持，请拆成多个标量端口")
        expanded.append(dict(p, dir="input", name=p["name"] + "_i"))
        expanded.append(dict(p, dir="output", tristate=True, name=p["name"],
                             data_sig=p["cpp"] + "__out", oe_sig=p["cpp"] + "__en"))
    ports = expanded

    # 沿触发的 always 才算时序。Verilator 会自己保存上一拍时钟做边沿检测，
    # 这里只用来决定元件类型，不影响每拍 eval 的驱动方式
    clocks = []
    for node in mod.get("stmtsp", []):
        if node.get("type") != "ALWAYS":
            continue
        # 未展开的引用在 JSON 里是 "UNLINKED" 字符串，不是列表
        for sentree in as_list(node.get("sentreep")):
            for item in as_list(sentree.get("sensesp")):
                if item.get("edgeType") not in ("POS", "NEG"):
                    continue
                for v in as_list(item.get("sensp")):
                    if v.get("type") == "VARREF" and v.get("name") not in clocks:
                        clocks.append(v.get("name"))
    return ports, clocks
