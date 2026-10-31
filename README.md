# Digital Circuit Simulation

[中文版](README-zh.md)

A digital circuit simulation framework with JIT-compiled execution. Build circuits from components (gates, flip-flops, arithmetic, memory, floating-point, etc.), describe them in JSON, and simulate at high speed via LLVM/Clang ORC JIT or TinyCC.

## Features

- **70+ built-in components**: gates, flip-flops, registers, counters, adders, multipliers, dividers, comparators, float32/float64 arithmetic, RAM, ROM, FIFO, barrel shifter, sign extension, priority encoder, multi-port RAM, plus RISC-V CPU, Wishbone bus, UART/CLINT/PLIC and other platform components
- **JIT compilation**: circuit → C code → native machine code, executed in memory
- **Dual backend**: LLVM/Clang ORC JIT (default) or TinyCC (lightweight alternative)
- **JSON import/export**: full circuit description, round-trip safe
- **Composite components**: reusable sub-circuits with shared definitions (e.g., define a half-adder once, instantiate 10 times)
- **DLL components**: load external Verilog/Verilator/C modules at runtime
- **Verilator integration**: Verilog → C++ → DLL → simulation (optional)
- **Configurable bit-widths**: 1–128 bits per wire
- **Combinational loop detection**: three-color DFS with cycle reporting
- **Parameter validation**: invalid values rejected at JSON load time
- **gtest suite**: 32 test targets covering all component types and the official riscv-tests

## Quick Start

### Windows (LLVM backend)

```bash
cmake -B build \
  -DLLVM_ROOT=E:/path/to/clang+llvm-22.1.7-x86_64-pc-windows-msvc \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --parallel --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

### Windows (TCC backend)

```bash
cmake -B build_tcc -DDCS_USE_TCC=ON \
  -DTCC_ROOT=E:/path/to/tinycc/win32 \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build_tcc --parallel --config RelWithDebInfo
ctest --test-dir build_tcc -C RelWithDebInfo --output-on-failure
```

`TCC_ROOT` is optional — if omitted, CMake searches system paths.

### Linux / WSL (TCC backend)

```bash
# Install TCC dev package (optional, otherwise set TCC_ROOT)
sudo apt install libtcc-dev

cmake -B build_tcc -DDCS_USE_TCC=ON \
  -DDCS_BUILD_VERILOG=OFF \
  -DCMAKE_TOOLCHAIN_FILE=/home/user/.local/share/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build_tcc --parallel
ctest --test-dir build_tcc --output-on-failure
```

### Verilator integration (optional)

Requires MSYS2/UCRT64 Verilator:

```bash
cmake -B build \
  -Dverilator_ROOT=E:/msys64/ucrt64/local \
  ... (other options)
```

Disable with `-DDCS_BUILD_VERILOG=OFF`.

### Generating components from Verilog

`tools/verilog2dcs.py` turns a Verilog module into a native dcs component
(Verilator model + generated wrapper), no DLL involved:

```bash
python tools/verilog2dcs.py verilog_test/adder8.v --type gen_adder8
cmake -B build ...            # re-run configure: new files are picked up by GLOB
cmake --build build --parallel 6
```

The wrapper exposes the module's ports as dcs pins. Direction, width and whether
the module is sequential are read from Verilator's exported AST, so the clock can
be named anything. Ports go up to 128 bits; unpacked arrays are expanded into
individual pins, and `inout` ports are split into `pad_i` + tristate `pad`
via Verilator's `--pins-inout-enables`, wired to the same net for bidirectional use.

### CLI Usage

```bash
# Simulate
./build/RelWithDebInfo/dcs circuit.json -t 20 -d 500 -m

# Convert JSON to C
./build/RelWithDebInfo/dcs --json-to-c circuit.json

# List all component types
./build/RelWithDebInfo/dcs --list-types

# Inspect DLL component
./build/RelWithDebInfo/dcs --dll-info adder8_dll.dll
```

## Using as a Subdirectory

```cmake
set(DCS_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DCS_BUILD_VERILOG OFF CACHE BOOL "" FORCE)
add_subdirectory(digital_circuit_simulation)

target_link_libraries(my_app PRIVATE dcs_lib)
```

## Component Types

### Combinational Logic

| Type | Description | Parameters |
|------|-------------|------------|
| `and`, `or`, `nand`, `nor`, `xor`, `xnor` | Multi-input gates | `inputs` (2–8), `bit_width` (1–64) |
| `not` | Bitwise NOT | `bit_width` |
| `abs` | Absolute value | `bit_width` |
| `min`, `max` | Min/max | `bit_width`, `signed` (0/1) |
| `redand`, `redor`, `redxor` | Reduction AND/OR/XOR | `bit_width` |
| `lsl`, `lsr`, `asr` | Shifts (amount from input pin) | `bit_width` |
| `buf` | Buffer | `bit_width` |
| `tsbuf` | Tri-state buffer (oe-controlled) | `bit_width` |
| `mux` | Multiplexer (2^N:1) | `n_selects` (1–4), `bit_width` |
| `adder` | Full adder: {cout, sum} = a + b + cin | `bit_width` |
| `sub` | Subtractor: {bout, diff} = a - b - bin | `bit_width` |
| `mul` | Multiplier: {prod_hi, prod_lo} = a × b | `bit_width` (1–64), `signed` (0/1) |
| `div` | Divider: {quot, rem} = a / b | `bit_width`, `signed` |
| `comparator` | Comparator | `bit_width`, `op` (eq/ne/lt/gt/le/ge) |
| `decoder` | N-bit → 2^N one-hot | `n_selects` (1–8) |
| `encoder` | 2^N one-hot → N-bit | `n_selects` (1–8) |
| `prienc` | Priority encoder | `num_inputs` (2–64) |
| `signext` | Sign extension | `src_width` (1–63), `dst_width` (2–64) |
| `barrel` | Barrel shifter | `bit_width` (2–64) |
| `constant` | Fixed integer value | `bit_width`, `value` |
| `switch` | Conditional pass-through | `bit_width` |
| `merge` | N × 1-bit → N-bit bus | `num_bits` (2–64) |
| `split` | N-bit bus → N × 1-bit | `num_bits` (2–64) |

### Floating-Point (IEEE 754)

| Type | Description | Parameters |
|------|-------------|------------|
| `fadd`, `fsub`, `fmul`, `fdiv` | Float32/64 arithmetic | `precision` (32/64) |
| `feq`, `fne`, `flt`, `fgt`, `fle`, `fge` | Float comparison | `precision` (32/64) |
| `f2i`, `i2f` | Float/integer conversion | `precision`, `int_width`, `signed` |
| `fconst` | Float constant | `precision`, `value` (e.g., "3.14") |

### Sequential Logic

| Type | Description | Parameters |
|------|-------------|------------|
| `dff` | D flip-flop (rising-edge) | `bit_width`, optional `has_en`/`has_rst`/`has_preset` |
| `tff` | T flip-flop (toggle) | `bit_width` |
| `jkff` | JK flip-flop | `bit_width` |
| `register` | Multi-bit register | `bit_width`, optional `has_en`/`has_rst` |
| `latch` | D latch (level-sensitive) | `bit_width` |
| `counter` | Up/down counter | `bit_width`, optional `has_load`/`has_en`/`has_updown`/`has_clr` |
| `shift` | Shift register | `length`, optional `has_dir`/`has_clr` |
| `clkgen` | Clock generator | `high_ticks`, `low_ticks` |
| `clkdiv` | Clock divider | `divisor` |
| `edgedet_rise`, `edgedet_fall`, `edgedet_both` | Edge detector | — |
| `delay` | N-stage delay line | `bit_width`, `depth` (1–256), `use_clock` (0/1) |

### Memory

| Type | Description | Parameters |
|------|-------------|------------|
| `memory` | RAM with read/write latency | `addr_width`, `data_width`, `read_latency`, `write_latency` |
| `rom` | ROM (hex / raw bytes / file mmap) | `addr_width`, `data_width`, `source_type` (hex/file), `filepath`, `use_mmap` |
| `mpram` | Multi-port RAM (1–4R + 1–2W, rd_valid flow control) | `addr_width`, `data_width`, `num_read_ports`, `num_write_ports`, `read_latency` |
| `fifo` | Synchronous FIFO | `data_width`, `depth` (power of 2), `has_rst` |

### RISC-V Platform

| Type | Description | Parameters |
|------|-------------|------------|
| `riscv_cpu` | RISC-V soft core, Wishbone master | `xlen` (32 or 64) |
| `wishbone_mem` | Wishbone slave memory | `addr_width` (1–24), `data_width` (8/16/32) |
| `wishbone_rom_adapter` | Wishbone adapter for ROM | `addr_width`, `data_width`, `read_latency` |
| `wishbone_ic` | Wishbone address decoder/router | `segments` (e.g. `0:65536,0x80000000:65536`), `num_masters` |
| `clint` | CLINT: mtime/mtimecmp/msip | — |
| `plic` | PLIC interrupt controller | `num_sources` |
| `uart` | 16550A-compatible UART | `baud_div` |
| `spi` / `i2c` | SPI / I2C master | `clkdiv` |
| `dma` | Memory-to-memory DMA | — |
| `axi_lite_bridge` | AXI-Lite ↔ Wishbone bridge | — |
| `wb_cache` | Wishbone cache | `lines` |

The CPU implements M/S/U privilege levels, Sv32/Sv39 paging, Zicsr/Zifencei, the
A/F/D/Zfh/Zba/Zbb/Zbc/Zbs/Zicond extensions, and Sdtrig hardware breakpoints
(mcontrol subset).

### Utility

| Type | Description | Parameters |
|------|-------------|------------|
| `print` | Print integer wire value | `bit_width` |
| `fprint` | Print float wire value | `precision` (32/64) |
| `dll` | External DLL component | `path` |
| `composite` | Reusable sub-circuit | `definition` (blueprint name) |

## Benchmarks

`benchmarks/` ships six ELF workloads (integer ALU, memory, branches, mul/div, double
precision FP, mixed) and two pure-circuit cases (256-stage gate chain, 256 D flip-flops)
to show throughput under different loads:

```bash
bash benchmarks/build.sh             # cross-compile the benchmark ELFs into benchmarks/out
./build/RelWithDebInfo/dcs_bench     # 40M ticks per case by default, best of 3 repeats
```

Output includes JIT compile time, ticks per second, completed cycles and the icache
hit rate; `--ticks N` changes the tick count, `--repeat R` the number of rounds,
`--dir` the ELF directory. Individual ELF paths may also be passed directly.

There are two ways to drive the clock, both shown by the benchmark:

- `circuit->tickCycles(clk_net_id, n)` — run n cycles inside the JIT, cheapest
- add a `clkgen` component and call `circuit->tick()` per tick — about 7% slower,
  the right choice when you need a divided or multi-domain clock

Reference numbers (Windows / clang 22, RelWithDebInfo). "Completed cycles" is the
number of clock cycles needed to finish the whole program, which reflects real speed
better than tick throughput:

| Case | Cycles | Time | Cycles/instr |
|------|--------|------|--------------|
| alu (integer) | 32.0M | 1.06 s | 1.0 |
| branch (branch-heavy) | 26.7M | 0.97 s | 1.0 |
| fp (double precision) | 32.0M | 1.15 s | 1.0 |
| mem (memory) | 4.6M | 0.13 s | 1.0 |
| mixed | 11.8M | 0.35 s | 1.0 |
| muldiv (mul/div) | 56.0M | 2.72 s | 3.5 |

Once the instruction is in the icache it costs exactly one cycle; muldiv is higher
because a 64-bit division itself takes several cycles. Compared with the initial
version on the same machine and configuration: alu went from 9.22 s to 1.06 s,
about 5–8× overall.

## JSON Circuit Format

```json
{
  "definitions": {
    "half_adder": {
      "nets": ["a", "b", "sum", "carry"],
      "components": [
        { "type": "xor", "name": "xor1", "params": {"inputs":"2","bit_width":"1"},
          "connections": {"in0":"a","in1":"b","out":"sum"} },
        { "type": "and", "name": "and1", "params": {"inputs":"2","bit_width":"1"},
          "connections": {"in0":"a","in1":"b","out":"carry"} }
      ],
      "expose": { "inputs": {"a":"a","b":"b"}, "outputs": {"sum":"sum","carry":"carry"} }
    }
  },
  "nets": ["a0", "b0", "s0", "c0"],
  "components": [
    { "type": "composite", "name": "ha", "params": {"definition":"half_adder"},
      "connections": {"a":"a0","b":"b0","sum":"s0","carry":"c0"} }
  ]
}
```

## Architecture

```
JSON / C++ API
      │
      ▼
  Circuit (nets + components container)
      │
      ├─ expandComposites()  — flatten sub-circuits
      ├─ check()             — combinational loop detection (3-color DFS)
      │
      ▼
  generateC()  — each component emits C code snippets
      │
      ▼
  JIT compile (LLVM Clang ORC / TinyCC)
      │
      ▼
  Executable: circuit_init / circuit_tick / set_wire / get_wire
```

- C++23, CMake 4.2
- LLVM/Clang 22.1.7: Clang API in-memory C → LLVM IR → ORC JIT
- TinyCC: libtcc in-memory C → direct execution
- gtest + vcpkg for testing
- `std::format` + raw string literals for C code generation
- Wire max width: 64 bits (16-byte internal slot)

## Adding a New Component

1. **Header** `include/dcs/components/<Name>.h` — inherit `CombinationalComponent` (combinational) or `SequentialComponent` (sequential)
2. **Implementation** `src/components/<Name>.cpp` — implement `genFuncDef_comb()` / `genFuncDef_seq()` + `clone()`
3. **Register** in `src/ComponentFactory.cpp` — `f.registerType(...)` in `InitAllComponents()`, add `#include`
4. **Test** in `tests/test_<name>/` — split `.cpp` by feature, follow existing patterns
5. Re-run `cmake -B build ...` so GLOB picks up new files

### Base Class Reference

| Base Class | `hasCombinational()` | `hasSequential()` | Must Implement |
|------------|-----------------------|--------------------|----------------|
| `CombinationalComponent` | true | false | `genFuncDef_comb()` |
| `SequentialComponent` | false | true | `genFuncDef_seq()`, `genStateDecl()`, `genInitCode()` |

### C Code Generation Helpers

- `gen_read_wire(net_id, pin_w, net_w, var)` — read wire value (auto `= 0`)
**Ordinary components (all logic emitted as C)**

- `gen_read_wire(net_id, pin_w, net_w, var)` — read a wire value (variable auto-initialized to `= 0`)
- `genOutputWrite(out_idx, val, pin_w, drive = "true")` — write an output pin. Non-tri-state outputs go
  straight into `_w[]` with the upper bytes cleared; tri-state outputs write `_c_out_` and set the enable
  from `drive`. **This is the default for new components**
- `gen_write_wire(comp_id, out_idx, val, pin_w)` — write `_c_out_` directly (only when you need to control
  tri-state timing yourself)

**High-performance paths for callback-style components (JIT only moves pins, logic lives in C++)**

These bypass the upper-byte clearing done by `genOutputWrite`; only callback components meeting the
preconditions may use them:

- `genInputPtrArray(arr)` — static pointer table over input wires, so the callback reads wires directly
- `genOutputPtrArray(count, clear)` — callback writes `_obuf`, which `genOutputWrite` then copies to the wires
  (safe default, costs one extra copy per tick)
- `genOutputWirePtrArray(arr)` — callback writes `_w[]` **directly** (maximum performance, skips that copy).
  Preconditions: singly-driven outputs, callback writes every output every tick, and never writes more bytes
  than the pin width
- `c_int_type(width)` — width → C type (uint8/16/32/64_t)
- `gen_mask(width)` — width → mask literal
- `byte_count(width)` — width → byte count

## License

MIT
