#!/usr/bin/env python3
"""把 Verilog 模块打包成运行时加载的 DLL 元件（不需要重新编译 dcs）。

用法:
    python tools/verilog2dll.py my_top.v [--top my_top] [--name my_top] [-o 输出目录]

和 verilog2dcs.py 的区别：verilog2dcs 生成 C++ 元件源码、编进库；这个脚本
用 Verilator 生成模型，再生成一个实现 dcs_dll_interface.h 的包装层和一个
独立的 CMake 工程。编译交给 CMake，用什么编译器（MSVC / clang / gcc）由你决定，
脚本本身不调用编译器。之后电路 JSON 里用 {"type": "dll", "params": {"path": ...}}
引用即可，改 Verilog 只需要重跑这个脚本。

生成到 <输出目录>（默认 verilog_dll/<top>/）:
    obj_dir/                 Verilator 模型和 AST（保留，便于排查/重建）
    <name>_wrapper.cpp       包装层（可以直接改；已存在时脚本不会覆盖）
    sc_time_stamp.cpp        Verilator 在 COFF 上需要的弱符号替身
    CMakeLists.txt           独立工程，产物固定落到 dist/
    <name>.json              把模块所有端口接成线网的冒烟电路，可直接 dcs 运行
    dist/<name>.dll          运行 `cmake -S . -B build && cmake --build build` 后出现
"""
import argparse
import os
import pathlib
import sys

from verilog_common import VERILATOR_INCLUDE, parse_design, verilate

WRAPPER_TEMPLATE = """// 由 tools/verilog2dll.py 从 Verilog 生成的起点，可以直接修改：
// 需要额外行为（复位策略、参数、调试输出等）就在这份代码上加。
// 脚本只在文件不存在时生成，已有文件不会被覆盖。
//
// 实现 include/dcs/dll_interface.h。每个引脚在输入/输出缓冲区里占 16 字节，
// 顺序和描述符里一致；DCS_DLL_EXPORT 负责把符号导出给 dcs 运行时加载。
#include <cstring>
#include <memory>
#include <new>
#include <stdint.h>

#include "V@TOP@.h"
#include "verilated.h"
#include "dcs/dll_interface.h"

namespace {
// 每个实例独立持有 VerilatedContext：同一个 DLL 被多个元件引用时，
// 模型状态（包括 Verilator 的边沿检测）不能互相串
struct State {
    std::unique_ptr<VerilatedContext> ctx = std::make_unique<VerilatedContext>();
    V@TOP@ m{ctx.get()};
};
} // namespace

@INPUT_ARR@
@OUTPUT_ARR@

extern "C" DCS_DLL_EXPORT dcs_dll_descriptor_t dcs_dll_desc = {
        "@TYPE@", @NIN@, @NOUT@, @IN_PTR@, @OUT_PTR@};

extern "C" DCS_DLL_EXPORT void dcs_dll_init(void) {}
extern "C" DCS_DLL_EXPORT void dcs_dll_deinit(void) {}

extern "C" DCS_DLL_EXPORT void *dcs_dll_create(void) {
    return new State;
}

extern "C" DCS_DLL_EXPORT void dcs_dll_destroy(void *p) {
    delete static_cast<State *>(p);
}

// 复位：Verilator 模型没有通用的 reset 接口，用析构 + placement new 回到
// 构造时的初值（initial 块在构造函数里执行），再 eval 一次刷新组合输出。
// 想保留状态或走 async reset 端口，直接改这个函数
extern "C" DCS_DLL_EXPORT void dcs_dll_reset(void *p, uint8_t *out) {
    auto *s = static_cast<State *>(p);
    s->m.~V@TOP@();
    s->ctx = std::make_unique<VerilatedContext>();
    new (&s->m) V@TOP@(s->ctx.get());
    V@TOP@ &m = s->m;
    s->m.eval();
@READ_OUT@
}

// 每拍调一次：写输入 → eval → 读输出。Verilator 自己记录上一拍时钟做边沿检测，
// 不能在外面加边沿门控，否则模型看不到下降沿，之后的上升沿都不成立
extern "C" DCS_DLL_EXPORT void dcs_dll_tick_@PHASE@(void *p, const uint8_t *in, uint8_t *out) {
    auto *s = static_cast<State *>(p);
    V@TOP@ &m = s->m;
@WRITE_IN@
    m.eval();
@READ_OUT@
}
"""

# Verilator 在不使用 SystemC 时约定由用户提供时间戳回调：verilated.cpp 以弱符号
# 引用 sc_time_stamp()，而 Windows 的 COFF 不支持弱符号，不给定义就直接链接失败。
# 故意不加 extern "C"：Verilator 头文件里的声明是 C++ 链接的
SC_TIME_STUB = """// 由 tools/verilog2dll.py 生成，给 verilated.cpp 提供 sc_time_stamp()
// Windows 的 COFF 没有弱符号，见 src/verilog_gen/verilated_stub.cpp 的说明
double sc_time_stamp() {
    return 0;
}
"""

CMAKE_TEMPLATE = """# 由 tools/verilog2dll.py 生成的独立工程，编译器/生成器随你选：
#   cmake -S . -B build
#   cmake --build build
# Ninja/Makefiles 这类单配置生成器默认用 Release；Visual Studio/Xcode 这类
# 多配置生成器在构建时自己选（cmake --build build --config Release）
# 产物固定输出到 dist/，和同名 .json 冒烟电路放在一起
cmake_minimum_required(VERSION 3.20)
project(verilog_dll_@TOP@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(VERILATOR_INCLUDE "@VERILATOR_INCLUDE@" CACHE PATH "Verilator include 目录")
set(DCS_INCLUDE "@DCS_INCLUDE@" CACHE PATH "dcs 的 include 目录（dll_interface.h）")

file(GLOB MODEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/obj_dir/V*.cpp")

add_library(@NAME@ SHARED
    @NAME@_wrapper.cpp
    sc_time_stamp.cpp
    ${MODEL_SOURCES}
    "${VERILATOR_INCLUDE}/verilated.cpp"
    "${VERILATOR_INCLUDE}/verilated_threads.cpp"
)

target_include_directories(@NAME@ PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/obj_dir"
    "${DCS_INCLUDE}"
    "${VERILATOR_INCLUDE}"
    "${VERILATOR_INCLUDE}/vltstd"
)

# 多配置生成器（VS）会给每个配置加子目录，这里全部固定到 dist/，
# JSON 里的相对路径才不用管 Debug/Release
set_target_properties(@NAME@ PROPERTIES
    OUTPUT_NAME "@NAME@"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(@NAME@ PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_CURRENT_SOURCE_DIR}/dist"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_CURRENT_SOURCE_DIR}/dist"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    )
endforeach()
"""


def gen_wrapper(top: str, type_name: str, ports, sequential: bool) -> str:
    ins = [p for p in ports if p["dir"] == "input"]
    outs = [p for p in ports if p["dir"] == "output"]

    def pin_arr(var, pins):
        if not pins:
            return f"static dcs_dll_pin_desc_t *{var} = nullptr;"
        rows = ",\n".join(
            f'    {{"{p["name"]}", {p["width"]}, {1 if p.get("tristate") else 0}, '
            f'{1 if (sequential and var == "s_outputs") else 0}}}'
            for p in pins)
        return f"static dcs_dll_pin_desc_t {var}[] = {{\n{rows}\n}};"

    write_in = "\n".join(
        f"    std::memcpy(&m.{p['cpp']}, in + {i * 16}, {(p['width'] + 7) // 8});"
        for i, p in enumerate(ins)) or "    // 模块没有输入"

    read_out = "\n".join(
        f"    std::memcpy(out + {i * 16}, &m.{p.get('data_sig', p['cpp'])}, {(p['width'] + 7) // 8});"
        for i, p in enumerate(outs)) or "    // 模块没有输出"

    return (WRAPPER_TEMPLATE
            .replace("@TOP@", top)
            .replace("@TYPE@", type_name)
            .replace("@PHASE@", "seq" if sequential else "comb")
            .replace("@INPUT_ARR@", pin_arr("s_inputs", ins))
            .replace("@OUTPUT_ARR@", pin_arr("s_outputs", outs))
            .replace("@NIN@", str(len(ins)))
            .replace("@NOUT@", str(len(outs)))
            .replace("@IN_PTR@", "s_inputs" if ins else "nullptr")
            .replace("@OUT_PTR@", "s_outputs" if outs else "nullptr")
            .replace("@WRITE_IN@", write_in)
            .replace("@READ_OUT@", read_out))


def gen_smoke_json(top: str, dll_rel: str, ports) -> str:
    nets = []
    conns = []
    for p in ports:
        if p["name"] not in nets:
            nets.append(p["name"])
        conns.append(f'        "{p["name"]}": "{p["name"]}"')
    net_lines = ",\n".join(f'    "{n}"' for n in nets)
    conn_text = ",\n".join(conns)
    return (f'{{\n'
            f'  "nets": [\n{net_lines}\n  ],\n'
            f'  "components": [\n'
            f'    {{"type": "dll", "name": "{top}",\n'
            f'     "params": {{"path": "{dll_rel}"}},\n'
            f'     "connections": {{\n{conn_text}\n     }}}}\n'
            f'  ]\n'
            f'}}\n')


def cmake_libs() -> str:
    # 和构建产物同名的动态库后缀；JSON 里用相对路径引用
    return ".dll" if sys.platform == "win32" else ".so"


def find_dcs_include() -> pathlib.Path:
    """定位 dcs 的 include 目录（dll_interface.h 所在处）。

    脚本既可能在源码树里（tools/../include），也可能被安装到
    <prefix>/share/dcs/tools 或 <prefix>/bin；两种布局都试一遍，
    最后允许用 DCS_INCLUDE 覆盖。
    """
    here = pathlib.Path(__file__).resolve().parent
    candidates = []
    if os.environ.get("DCS_INCLUDE"):
        candidates.append(pathlib.Path(os.environ["DCS_INCLUDE"]))
    candidates.append(here.parent / "include")       # 源码树 tools/../
    candidates.append(here / ".." / ".." / "include")  # bin/ 安装布局
    candidates.append(here / ".." / ".." / ".." / "include")  # share/dcs/tools 安装布局
    for c in candidates:
        if (c / "dcs" / "dll_interface.h").is_file():
            return c.resolve()
    raise SystemExit("找不到 dcs 的 include 目录（dll_interface.h），请设置 DCS_INCLUDE")


def main():
    ap = argparse.ArgumentParser(description="把 Verilog 模块打包成 dcs 运行时加载的 DLL 元件")
    ap.add_argument("vfile")
    ap.add_argument("--top", default=None, help="顶层模块名（默认取文件名）")
    ap.add_argument("--name", default=None, help="DLL 文件名/元件实例名（默认取顶层模块名）")
    ap.add_argument("--type", default=None, dest="type_name", help="描述符里的元件类型名")
    ap.add_argument("-o", "--outdir", default=None, help="输出目录（默认 verilog_dll/<top>）")
    args = ap.parse_args()

    src = pathlib.Path(args.vfile)
    top = args.top or src.stem
    name = args.name or top
    type_name = args.type_name or top
    out = pathlib.Path(args.outdir) if args.outdir else pathlib.Path("verilog_dll") / top
    out.mkdir(parents=True, exist_ok=True)
    obj_dir = out / "obj_dir"

    ast, header = verilate(src, top, obj_dir)
    ports, clocks = parse_design(ast, top)

    # 和 verilog2dcs 同样的检查：Verilator 没拆开的 inout 无法生成
    header_text = header.read_text(encoding="utf-8", errors="replace")
    for p in ports:
        if p.get("tristate") and p["oe_sig"] not in header_text:
            raise SystemExit(
                f"Verilator 没有把 inout 端口 {p['name']} 拆成 __en/__out，"
                f"它只支持 \"assign 端口 = 使能 ? 数据 : 'z\" 这类简单三态结构")
        if p["width"] > 128:
            raise SystemExit(
                f"端口 {p['name']} 位宽 {p['width']} 超过 dcs 线网上限 128 位（16 字节槽）")
    if not any(p["dir"] == "output" for p in ports):
        raise SystemExit("模块没有输出")

    sequential = bool(clocks)
    wrapper = out / f"{name}_wrapper.cpp"
    if wrapper.exists():
        print(f"包装层已存在，保留: {wrapper}（删掉它可重新生成）")
    else:
        wrapper.write_text(gen_wrapper(top, type_name, ports, sequential), encoding="utf-8")
    (out / "sc_time_stamp.cpp").write_text(SC_TIME_STUB, encoding="utf-8")

    project_include = find_dcs_include()
    (out / "CMakeLists.txt").write_text(
        CMAKE_TEMPLATE
        .replace("@TOP@", top)
        .replace("@NAME@", name)
        .replace("@VERILATOR_INCLUDE@", str(VERILATOR_INCLUDE).replace("\\", "/"))
        .replace("@DCS_INCLUDE@", str(project_include).replace("\\", "/")),
        encoding="utf-8")

    dll_rel = f"dist/{name}{cmake_libs()}"
    json_path = out / f"{name}.json"
    json_path.write_text(gen_smoke_json(top, dll_rel, ports), encoding="utf-8")

    port_desc = ", ".join(f'{p["dir"]}:{p["name"]}({p["width"]})' for p in ports)
    print(f"工程:   {out / 'CMakeLists.txt'}")
    print(f"包装层: {wrapper}")
    print(f"电路:   {json_path}")
    print(f"产物:   {out / dll_rel}")
    print(f"端口:   {port_desc}")
    print(f"类型:   {'时序（时钟: ' + ', '.join(clocks) + '）' if sequential else '组合'}")
    print(f"构建:   cmake -S \"{out}\" -B \"{out / 'build'}\" 然后 cmake --build \"{out / 'build'}\"")


if __name__ == "__main__":
    main()
