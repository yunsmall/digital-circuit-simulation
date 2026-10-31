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
import os
import pathlib
import re
import shutil
import subprocess
import sys

VERILATOR_BIN = os.environ.get("VERILATOR_BIN", "E:/msys64/ucrt64/local/bin/verilator_bin.exe")
VERILATOR_ROOT = os.environ.get("VERILATOR_ROOT", "E:/msys64/ucrt64/local/share/verilator")

PORT_RE = re.compile(r"VL_(IN|OUT|INOUT)(\d*)\s*\(\s*&(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)")
PORT_DIR = {"IN": "input", "OUT": "output", "INOUT": "inout"}
CLOCK_NAMES = ("clk", "clock", "clk_i", "i_clk", "clk_in")


def verilate(src: pathlib.Path, top: str, workdir: pathlib.Path) -> pathlib.Path:
    workdir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["VERILATOR_ROOT"] = VERILATOR_ROOT
    env.setdefault("LC_ALL", "C")
    cmd = [VERILATOR_BIN, "--cc", "--Mdir", str(workdir), "--top-module", top, str(src)]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("verilator 失败")
    header = workdir / f"V{top}.h"
    if not header.exists():
        raise SystemExit(f"没有生成 {header}（--top 是否和模块名一致？）")
    return header


def parse_ports(header: pathlib.Path):
    text = header.read_text(encoding="utf-8", errors="replace")
    ports = []
    for kind, _w, name, msb, lsb in PORT_RE.findall(text):
        width = int(msb) - int(lsb) + 1
        ports.append({"dir": PORT_DIR[kind], "name": name, "width": width})
    if not ports:
        raise SystemExit("没有解析到端口（>64 位的 wide 端口暂不支持）")
    for p in ports:
        if p["width"] > 64:
            raise SystemExit(f"端口 {p['name']} 位宽 {p['width']} 超过 64，暂不支持")
    return ports


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

    write_in = [f"    m->{p['name']} = ({ctype(p['width'])}) in[{i}];"
                for i, p in enumerate(ins)]
    read_out = [f"    out[{i}] = ({ctype(p['width'])}) m->{p['name']};"
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

    header = verilate(src, top, work)
    ports = parse_ports(header)
    if any(p["dir"] == "inout" for p in ports):
        raise SystemExit("inout 端口暂不支持")
    ins = [p for p in ports if p["dir"] == "input"]
    comb = not any(p["name"].lower() in CLOCK_NAMES for p in ins)
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
    print(f"类型: {'组合' if comb else '时序'}元件")


if __name__ == "__main__":
    main()
