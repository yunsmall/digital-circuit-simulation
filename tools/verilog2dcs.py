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


def verilate(src: pathlib.Path, top: str, workdir: pathlib.Path):
    """生成模型源码，并额外导出一份 AST（端口/时序信息都从 AST 读）。"""
    # --pins-inout-enables：顶层 inout 会被拆成 input + <name>__en + <name>__out，
    # 这是 Verilator 官方支持的双向端口建模方式，正好对应 dcs 的三态总线
    common = ["--cc", "--pins-inout-enables", "--top-module", top, src]
    run_verilator([*common, "--Mdir", workdir], workdir)
    header = workdir / f"V{top}.h"
    if not header.exists():
        raise SystemExit(f"没有生成 V{top}.h（--top 是否和模块名一致？）")

    # --json-only-output 本身不产出 C++，所以要和上一步分开跑
    ast = workdir / "ast.json"
    run_verilator([*common, "--json-only-output", ast, "--Mdir", workdir / "ast"], workdir)
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
#include <cstring>
#include <format>
#include <memory>
#include <verilated.h>

namespace dsc {

struct @NAME@::State {
    VerilatedContext ctx;
    std::unique_ptr<V@TOP@> model;
};

// __en 是按位给出驱动使能的，而 dcs 的三态是整根线驱动；只要有一位使能就整根线驱动。
// 全 0 / 全 1 是常见情形，逐位混合的驱动 dcs 表达不了
static bool dcsAnyBits(const void *p, std::size_t n) {
    auto *b = static_cast<const unsigned char *>(p);
    for (std::size_t i = 0; i < n; i++)
        if (b[i])
            return true;
    return false;
}

// 端口在 Verilator 模型里是引用成员（VL_IN8 展开成 "CData &a"），
// 没法用对象内偏移访问，只能每拍走一次这个回调搬引脚。
// 每个端口在缓冲区里占固定 16 字节（dcs 线网槽宽度），窄端口只用到前几字节
static void dcsEval@NAME@(void *p, const unsigned char *in, unsigned char *out) {
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
            "extern void dcs_eval_@TYPE@(void *, const unsigned char *, unsigned char *);\\n",
            _id, (unsigned long long) (std::uintptr_t) _state->model.get());
}

std::string @NAME@::genFuncDef_@KIND@() const {
    std::string c;
    c += std::format("DCS_INLINE void {}() {{\\n", funcName_@KIND@());
    c += std::format("    static unsigned char _vio_in_{0}[@NINS_BYTES@];\\n", _id);
    c += std::format("    static unsigned char _vio_out_{0}[@NOUTS_BYTES@];\\n", _id);
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
    def add_out(p):
        ts = bool(p.get("tristate"))
        if comb:
            return (f'    addOutput("{p["name"]}", {p["width"]}, true);' if ts
                    else f'    addOutput("{p["name"]}", {p["width"]});')
        # 时序输出跨拍可见，必须标成 sequential，否则环路检测会把它们当组合路径
        return (f'    addOutput("{p["name"]}", {p["width"]}, true, true);' if ts
                else f'    addOutput("{p["name"]}", {p["width"]}, false, true);')

    pins += [add_out(p) for p in outs]

    # 引脚名用 Verilog 原名；访问模型成员必须用 Verilator 生成的 C++ 名，
    # 端口名撞上 C++ 关键字时两者会不一样
    # 每个端口在缓冲区里占 16 字节，窄端口只用到前几字节
    write_in = [f"    std::memcpy(&m->{p['cpp']}, in + {i * 16}, {(p['width'] + 7) // 8});"
                for i, p in enumerate(ins)]

    # 三态输出要多占一个槽放使能，槽位在这里统一分配
    out_layout = []
    slot = 0
    for p in outs:
        val_off = slot * 16
        slot += 1
        oe_off = None
        if p.get("tristate"):
            oe_off = slot * 16
            slot += 1
        out_layout.append((p, val_off, oe_off))

    read_out = []
    for p, val_off, oe_off in out_layout:
        if oe_off is None:
            read_out.append(f"    std::memcpy(out + {val_off}, &m->{p['cpp']}, {(p['width'] + 7) // 8});")
        else:
            # 值和使能分别来自模型里的数据信号、三态条件信号
            read_out.append(
                f"    std::memcpy(out + {val_off}, &m->{p['data_sig']}, {(p['width'] + 7) // 8});")
            read_out.append(
                f"    out[{oe_off}] = dcsAnyBits(&m->{p['oe_sig']}, {(p['width'] + 7) // 8});")

    load = []
    for i, p in enumerate(ins):
        nbytes = (p["width"] + 7) // 8
        load.append(
            f'    c += std::format("    dcs_memcpy(_vio_in_{{0}} + {i * 16}, _w[{{1}}], {nbytes});\\n", '
            f'_id, inputs()[{i}]->netId());')

    store = []
    for i, (p, val_off, oe_off) in enumerate(out_layout):
        # 输出槽本身就是字节缓冲，genOutputWrite 直接对它取地址
        if oe_off is None:
            store.append(
                f'    c += "    " + genOutputWrite({i}, '
                f'std::format("_vio_out_{{}}[{val_off}]", _id), {p["width"]}) + "\\n";')
        else:
            store.append(
                f'    c += std::format("    const uint8_t _oe{i} = _vio_out_{{0}}[{oe_off}];\\n", _id);')
            store.append(
                f'    c += "    " + genOutputWrite({i}, '
                f'std::format("_vio_out_{{}}[{val_off}]", _id), {p["width"]}, "_oe{i}") + "\\n";')

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
            .replace("@NINS_BYTES@", str(max(1, len(ins)) * 16))
            .replace("@NOUTS_BYTES@", str(max(1, slot) * 16))
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

    ast, header = verilate(src, top, work)
    ports, clocks = parse_design(ast, top)

    # Verilator 只把"简单三态结构"拆成 input+__en+__out；没拆开的话端口里不会有 __en，
    # 生成出来的代码会编译失败，不如在这里给出明确原因
    header_text = header.read_text(encoding="utf-8", errors="replace")
    for p in ports:
        if p.get("tristate") and p["oe_sig"] not in header_text:
            raise SystemExit(
                f"Verilator 没有把 inout 端口 {p['name']} 拆成 __en/__out，"
                f"它只支持 \"assign 端口 = 使能 ? 数据 : 'z\" 这类简单三态结构")

    for p in ports:
        if p["width"] > 128:
            raise SystemExit(
                f"端口 {p['name']} 位宽 {p['width']} 超过 dcs 线网上限 128 位（16 字节槽），"
                f"需要手工拆成多个 ≤128 位的端口")

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
