#!/usr/bin/env python3
"""把 Verilog 模块生成成 dcs 元件（.h + .cpp）。

用法:
    python tools/verilog2dcs.py verilog_test/adder8.v [--top adder8] [--name Adder8] [--type adder8]

流程:
    1. verilator_bin 导出 AST（VERILATOR_ROOT 指向 share/verilator），顶层模块从
       AST 里"没被其它模块实例化"的模块判定，--top 只是显式覆盖；
    2. 端口名、方向、位宽、以及有没有沿触发的 always 全部从 AST 读；
    3. 有沿触发的 always -> 时序元件（每拍 eval 一次），否则组合元件；
    4. 元件每拍把输入打包进数组、调一次 C++ 回调（写端口 -> eval -> 读端口），
       再按输出引脚宽度写回线网。热路径上每个元件每拍只有一次跨 JIT 调用。

生成到:
    include/dcs/components/verilog/<Name>.h
    src/verilog_gen/<Name>.cpp        （放在 GLOB 之外，由 CMake 的 vgen target 编译）
    verilog_gen/<top>/<源文件>.v
"""
import argparse
import pathlib
import re
import shutil

from verilog_common import parse_design, verilate


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
    # 顶层交给 Verilator/AST 判定，不能按文件名猜：--top 只是显式覆盖
    work = pathlib.Path(args.workdir) if args.workdir else \
        pathlib.Path("build_verilog_gen") / (args.top or src.stem)

    ast, header, top = verilate(src, args.top, work)
    name = args.name or camel(top)
    type_name = args.type_name or top.lower()
    out = pathlib.Path(args.outdir)
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
