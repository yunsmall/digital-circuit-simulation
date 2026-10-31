#!/usr/bin/env python3
"""把 Verilog 模块生成成 dcs 元件（.h + .cpp）。

用法:
    python tools/verilog2dcs.py verilog_test/adder8.v [--top adder8] [--name Adder8] [--type adder8]

流程:
    1. verilator_bin --cc 生成 V<Top>.h（VERILATOR_ROOT 指向 share/verilator）；
    2. 从 V<Top>.h 的 VL_IN/VL_OUT 宏提取端口名、方向、位宽；
    3. 有 clk/clock 端口 -> 时序元件（在时钟上升沿 eval 一次），否则组合元件；
    4. 元件每拍把输入打包进数组、调一次 C++ 回调（写端口 -> eval -> 读端口），
       再按输出引脚宽度写回线网。热路径上每个元件每拍只有一次跨 JIT 调用。

生成到:
    include/dcs/components/verilog/<Name>.h
    src/verilog_gen/<Name>.cpp        （放在 GLOB 之外，由 CMake 的 vgen target 编译）
    verilog_gen/<top>/<源文件>.v
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

VERILATOR_BIN = os.environ.get("VERILATOR_BIN", "E:/msys64/ucrt64/local/bin/verilator_bin.exe")
VERILATOR_ROOT = os.environ.get("VERILATOR_ROOT", "E:/msys64/ucrt64/local/share/verilator")

DIR_MAP = {"INPUT": "input", "OUTPUT": "output", "INOUT": "inout"}


def run_verilator(args, workdir: pathlib.Path) -> None:
    workdir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["VERILATOR_ROOT"] = VERILATOR_ROOT
    env.setdefault("LC_ALL", "C")
    r = subprocess.run([VERILATOR_BIN, *map(str, args)], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("verilator 失败")


def verilate(src: pathlib.Path, top: str, workdir: pathlib.Path) -> pathlib.Path:
    """生成模型源码，并额外导出一份 AST（端口/时序信息都从 AST 读）。"""
    run_verilator(["--cc", "--Mdir", workdir, "--top-module", top, src], workdir)
    if not (workdir / f"V{top}.h").exists():
        raise SystemExit(f"没有生成 V{top}.h（--top 是否和模块名一致？）")

    # --json-only-output 本身不产出 C++，所以要和上一步分开跑
    ast = workdir / "ast.json"
    run_verilator(["--cc", "--json-only-output", ast, "--Mdir", workdir / "ast",
                   "--top-module", top, src], workdir)
    return ast


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
        width = type_width(types, node.get("dtypep", ""), node.get("verilogName", "?"))
        ports.append({
            "dir": DIR_MAP.get(node.get("direction", ""), "unknown"),
            "name": node.get("verilogName") or node.get("name"),  # 引脚名用 Verilog 原名
            "cpp": node.get("name"),                              # 模型成员名可能与原名不同
            "width": width,
        })
    if not ports:
        raise SystemExit("顶层模块没有端口")

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


def ctype(width: int) -> str:
    if width <= 8:
        return "uint8_t"
    if width <= 16:
        return "uint16_t"
    if width <= 32:
        return "uint32_t"
    return "uint64_t"


def camel(name: str) -> str:
    return "".join(p[:1].upper() + p[1:] for p in re.split(r"[_\-]+", name) if p)


HEADER_TEMPLATE = """// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "dcs/Component.h"

namespace dsc {

// 直接继承 Component：要把 Verilator 模型指针放进 genStateDecl，
// 而 CombinationalComponent 把 genStateDecl 标成了 final
class @NAME@ : public Component {
public:
    explicit @NAME@(const std::string &name);
    ~@NAME@() override;

    bool hasCombinational() const override { return @IS_COMB@; }
    bool hasSequential() const override { return @IS_SEQ@; }
    std::string genStateDecl() const override;
    std::string genFuncDef_@KIND@() const override;
    std::vector<JitSymbol> extraJitSymbols() const override;
    std::unique_ptr<Component> clone(const std::string &new_name) const override;

    struct State;

private:
    std::unique_ptr<State> _state;
};

} // namespace dsc
"""


SOURCE_TEMPLATE = """// 由 tools/verilog2dcs.py 从 Verilog 自动生成，请勿手工修改
#include "dcs/components/verilog/@NAME@.h"

#include "V@TOP@.h"
#include "dcs/ComponentFactory.h"

#include <cstdint>
#include <format>
#include <memory>
#include <verilated.h>

namespace dsc {

struct @NAME@::State {
    VerilatedContext ctx;
    std::unique_ptr<V@TOP@> model;
};

// 端口在 Verilator 模型里是引用成员（VL_IN8 展开成 "CData &a"），
// 没法用对象内偏移访问，只能每拍走一次这个回调搬引脚
static void dcsEval@NAME@(void *p, const uint64_t *in, uint64_t *out) {
    auto *m = static_cast<V@TOP@ *>(p);
@WRITE_IN@
    m->eval();
@READ_OUT@
}

@NAME@::@NAME@(const std::string &name) :
    Component(name, "@TYPE@"), _state(std::make_unique<State>()) {
    _state->model = std::make_unique<V@TOP@>(&_state->ctx, "@TOP@");
@PINS@
}

@NAME@::~@NAME@() = default;

std::vector<Component::JitSymbol> @NAME@::extraJitSymbols() const {
    return {{"dcs_eval_@TYPE@", reinterpret_cast<void *>(&dcsEval@NAME@)}};
}

std::string @NAME@::genStateDecl() const {
    return std::format(
            "static void *_vmd_{0} = (void *)0x{1:X}ULL;\\n"
            "extern void dcs_eval_@TYPE@(void *, const uint64_t *, uint64_t *);\\n",
            _id, (unsigned long long) (std::uintptr_t) _state->model.get());
}

std::string @NAME@::genFuncDef_@KIND@() const {
    std::string c;
    c += std::format("DCS_INLINE void {}() {{\\n", funcName_@KIND@());
    c += std::format("    static uint64_t _vio_in_{0}[@NINS@];\\n", _id);
    c += std::format("    static uint64_t _vio_out_{0}[@NOUTS@];\\n", _id);
@LOAD@
@EVAL@
@STORE@
    c += "}\\n";
    return c;
}

std::unique_ptr<Component> @NAME@::clone(const std::string &new_name) const {
    return std::make_unique<@NAME@>(new_name);
}

} // namespace dsc

// ---- 工厂注册 ----
namespace {
DCS_REGISTER_COMPONENT(
    {"@TYPE@", {}, @META_IN@, @META_OUT@, dsc::ComponentCategory::Misc,
     "由 Verilog 模块 @TOP@ 自动生成的元件"},
    [](const std::string &n, const std::unordered_map<std::string, std::string> &) -> std::unique_ptr<dsc::Component> {
        return std::make_unique<dsc::@NAME@>(n);
    });
} // namespace
"""


def gen_header(name: str, comb: bool) -> str:
    return (HEADER_TEMPLATE
            .replace("@NAME@", name)
            .replace("@KIND@", "comb" if comb else "seq")
            .replace("@IS_COMB@", "true" if comb else "false")
            .replace("@IS_SEQ@", "false" if comb else "true"))


def gen_source(name: str, type_name: str, top: str, ports, comb: bool) -> str:
    ins = [p for p in ports if p["dir"] == "input"]
    outs = [p for p in ports if p["dir"] == "output"]

    pins = [f'    addInput("{p["name"]}", {p["width"]});' for p in ins]
    if comb:
        pins += [f'    addOutput("{p["name"]}", {p["width"]});' for p in outs]
    else:
        # 时序输出跨拍可见，必须标成 sequential，否则环路检测会把它们当组合路径
        pins += [f'    addOutput("{p["name"]}", {p["width"]}, false, true);' for p in outs]

    # 引脚名用 Verilog 原名；访问模型成员必须用 Verilator 生成的 C++ 名，
    # 端口名撞上 C++ 关键字时两者会不一样
    write_in = [f"    m->{p['cpp']} = ({ctype(p['width'])}) in[{i}];"
                for i, p in enumerate(ins)]
    read_out = [f"    out[{i}] = ({ctype(p['width'])}) m->{p['cpp']};"
                for i, p in enumerate(outs)]

    load = []
    for i, p in enumerate(ins):
        nbytes = (p["width"] + 7) // 8
        load.append(
            f'    c += std::format("    uint64_t _i{i} = 0; dcs_memcpy(&_i{i}, _w[{{1}}], {nbytes}); '
            f'_vio_in_{{0}}[{i}] = _i{i};\\n", _id, inputs()[{i}]->netId());')

    store = []
    for i, p in enumerate(outs):
        store.append(
            f'    c += std::format("    const {ctype(p["width"])} _o{i} = '
            f'({ctype(p["width"])}) _vio_out_{{0}}[{i}];\\n", _id);')
        store.append(f'    c += "    " + genOutputWrite({i}, "_o{i}", {p["width"]}) + "\\n";')

    if comb:
        evals = [
            f'    c += std::format("    dcs_eval_{type_name}(_vmd_{{0}}, _vio_in_{{0}}, _vio_out_{{0}});\\n", _id);',
        ]
    else:
        # Verilator 模型内部自己记录时钟上一拍的值来做边沿检测，所以每一拍
        # 都必须把输入（含 clk）写进去再 eval。在外面加边沿门控会让模型
        # 永远看不到下降沿，下一个上升沿也就不算沿了
        evals = [
            f'    c += std::format("    dcs_eval_{type_name}(_vmd_{{0}}, _vio_in_{{0}}, _vio_out_{{0}});\\n", _id);',
        ]
    body_load = load
    body_store = store

    return (SOURCE_TEMPLATE
            .replace("@NAME@", name)
            .replace("@TYPE@", type_name)
            .replace("@TOP@", top)
            .replace("@KIND@", "comb" if comb else "seq")
            .replace("@META_IN@", "{" + ", ".join(f'"{p["name"]}"' for p in ins) + "}")
            .replace("@META_OUT@", "{" + ", ".join(f'"{p["name"]}"' for p in outs) + "}")
            .replace("@WRITE_IN@", "\n".join(write_in) if write_in else "    // 模块没有输入")
            .replace("@READ_OUT@", "\n".join(read_out) if read_out else "    // 模块没有输出")
            .replace("@PINS@", "\n".join(pins) if pins else "    // 模块没有端口")
            .replace("@NINS@", str(len(ins)))
            .replace("@NOUTS@", str(len(outs)))
            .replace("@LOAD@", "\n".join(body_load))
            .replace("@EVAL@", "\n".join(evals))
            .replace("@STORE@", "\n".join(body_store)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vfile")
    ap.add_argument("--top", default=None)
    ap.add_argument("--name", default=None)
    ap.add_argument("--type", default=None, dest="type_name")
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()

    src = pathlib.Path(args.vfile)
    top = args.top or src.stem
    name = args.name or camel(top)
    type_name = args.type_name or top.lower()
    out = pathlib.Path(args.outdir)
    work = pathlib.Path(args.workdir) if args.workdir else pathlib.Path("build_verilog_gen") / top

    ast = verilate(src, top, work)
    ports, clocks = parse_design(ast, top)

    for p in ports:
        if p["dir"] == "inout":
            raise SystemExit(
                f"inout 端口 {p['name']} 无法自动映射：dcs 的线网是单向的，"
                f"需要手工拆成 out/oe/in 三个引脚后再接 tsbuf")
        if p["width"] > 64:
            raise SystemExit(
                f"端口 {p['name']} 位宽 {p['width']} 超过 dcs 线网上限 64 位，"
                f"需要手工拆成多个 ≤64 位的端口")

    comb = not clocks
    if not any(p["dir"] == "output" for p in ports):
        raise SystemExit("模块没有输出")

    hpath = out / "include/dcs/components/verilog" / f"{name}.h"
    cpath = out / "src/verilog_gen" / f"{name}.cpp"
    hpath.parent.mkdir(parents=True, exist_ok=True)
    cpath.parent.mkdir(parents=True, exist_ok=True)
    hpath.write_text(gen_header(name, comb), encoding="utf-8")
    cpath.write_text(gen_source(name, type_name, top, ports, comb), encoding="utf-8")

    vdir = out / "verilog_gen" / top
    vdir.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, vdir / src.name)
    # CMake 靠这个清单把元件 cpp 和 Verilator 模型编进同一个 target
    (vdir / "dcs_component.txt").write_text(name + "\n", encoding="utf-8")

    port_desc = ", ".join(f'{p["dir"]}:{p["name"]}({p["width"]})' for p in ports)
    print(f"生成 {hpath}")
    print(f"生成 {cpath}")
    print(f"复制 {vdir / src.name}")
    print(f"端口: {port_desc}")
    if comb:
        print("类型: 组合元件（AST 里没有沿触发的 always）")
    else:
        print(f"类型: 时序元件（时钟: {', '.join(clocks)}）")


if __name__ == "__main__":
    main()
