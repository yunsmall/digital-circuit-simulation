# Digital Circuit Simulation

A digital circuit simulation framework with JIT-compiled execution. Build circuits from components (gates, flip-flops, arithmetic, memory, floating-point, etc.), describe them in JSON, and simulate at high speed via LLVM/Clang ORC JIT or TinyCC.

## Features

- **40+ built-in components**: gates, flip-flops, registers, counters, adders, multipliers, dividers, comparators, float32/float64 arithmetic, RAM, ROM, clock generator, print monitor, and more
- **JIT compilation**: circuit → C code → native machine code, executed in memory
- **Dual backend**: LLVM/Clang ORC JIT (default) or TinyCC (lightweight alternative)
- **JSON import/export**: full circuit description as JSON, round-trip safe
- **Composite components**: reusable sub-circuits with shared definitions (e.g., define a half-adder once, instantiate 10 times)
- **DLL components**: load external Verilog/Verilator/C modules at runtime
- **Configurable bit-widths**: 1–64 bits per wire
- **Combinational loop detection**: three-color DFS with cycle reporting
- **gtest test suite**: 9 test files covering all component types

## Quick Start

```bash
# Configure (requires vcpkg)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --parallel --config RelWithDebInfo

# Run tests
ctest --test-dir build -C RelWithDebInfo --output-on-failure

# Simulate a circuit
./build/RelWithDebInfo/digital_circuit_simulation circuit.json -t 20 -d 500

# Convert JSON to C code
./build/RelWithDebInfo/dcs_tool --json-to-c circuit.json > circuit.c

# List all component types
./build/RelWithDebInfo/dcs_tool --list-types
```

## Prerequisites

- C++23 compiler (MSVC 2022, Clang 18+, GCC 14+)
- CMake 3.28+
- [vcpkg](https://github.com/microsoft/vcpkg) packages: `nlohmann-json`, `cxxopts`, `gtest` (optional)
- LLVM/Clang 22.1.7 (default backend) or TinyCC (alternative)

## Component Types

### Combinational Logic

| Name | Description | Parameters |
|------|-------------|------------|
| `and`, `or`, `nand`, `nor`, `xor`, `xnor` | Multi-input gates | `inputs` (2–8), `bit_width` (1–64) |
| `not` | Bitwise NOT | `bit_width` |
| `buf` | Buffer | `bit_width` |
| `tsbuf` | Tri-state buffer (oe-controlled) | `bit_width` |
| `mux` | Multiplexer (2^N:1) | `n_selects` (1–4), `bit_width` |
| `adder` | Full adder: {cout, sum} = a + b + cin | `bit_width` |
| `sub` | Subtractor: {bout, diff} = a - b - bin | `bit_width` |
| `mul` | Unsigned/signed multiplier: prod = a × b | `bit_width` (≤32), `signed` |
| `div` | Unsigned/signed divider: {quot, rem} = a / b | `bit_width` (≤64), `signed` |
| `comparator` | Equality/relational comparison | `bit_width`, `op` (eq/ne/lt/gt/le/ge), `signed` |
| `decoder` | N-bit binary → 2^N one-hot | `n_selects` (1–8) |
| `encoder` | 2^N one-hot → N-bit binary | `n_selects` (1–8) |
| `constant` | Fixed integer value | `bit_width`, `value` |
| `switch` | Conditional pass-through | `bit_width` |
| `merge` | N × 1-bit → N-bit bus | `num_bits` (2–64) |
| `split` | N-bit bus → N × 1-bit | `num_bits` (2–64) |

### Floating-Point (IEEE 754)

| Name | Description | Parameters |
|------|-------------|------------|
| `fadd`, `fsub`, `fmul`, `fdiv` | Float32/64 arithmetic | `precision` (32 or 64) |
| `fcmp` | Float comparison | `precision`, `op` (eq/ne/lt/gt/le/ge) |
| `fconst` | Float constant | `precision`, `value` (e.g., "3.14") |

### Sequential Logic

| Name | Description | Parameters |
|------|-------------|------------|
| `dff` | D flip-flop (rising-edge) | `bit_width`, optional `has_en`/`has_rst`/`has_preset` |
| `tff` | T flip-flop (toggle) | `bit_width` |
| `jkff` | JK flip-flop | `bit_width` |
| `register` | Multi-bit register | `bit_width`, optional `has_en`/`has_rst` |
| `latch` | D latch (level-sensitive) | `bit_width` |
| `counter` | Up/down counter | `bit_width`, optional load/enable/direction/clear |
| `shift` | Shift register | `length`, optional direction/clear |
| `clkgen` | Clock generator (configurable duty cycle) | `high_ticks`, `low_ticks` |

### Memory

| Name | Description | Parameters |
|------|-------------|------------|
| `memory` | RAM with read/write latency | `addr_width`, `data_width`, `read_latency`, `write_latency` |
| `rom` | Read-only memory (hex-initialized) | `addr_width`, `data_width`, `initial_data` (hex), `read_latency` |

### Utility

| Name | Description | Parameters |
|------|-------------|------------|
| `print` | Print input value on clock edge | `bit_width` |
| `dll` | External DLL component | `path` |
| `composite` | Reusable sub-circuit | `definition` (blueprint name) |

## JSON Circuit Format

```json
{
  "definitions": {
    "half_adder": {
      "nets": ["a", "b", "sum", "carry"],
      "components": [
        { "type": "xor", "name": "xor", "params": {"inputs":"2","bit_width":"1"},
          "connections": {"in0":"a","in1":"b","out":"sum"} },
        { "type": "and", "name": "and", "params": {"inputs":"2","bit_width":"1"},
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

Convert to C with `dcs_tool --json-to-c`, or simulate directly with `digital_circuit_simulation circuit.json`.

## Architecture

```
JSON / C++ API
      │
      ▼
  Circuit (container: nets + components)
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
  Executable functions: circuit_init / circuit_tick / set_wire / get_wire
```

### Adding a New Component

1. Declare class in `include/dcs/components/<Name>.h`
2. Implement `genFuncDef()` + `clone()` in `src/components/<Name>.cpp`
3. Register in `src/ComponentFactory.cpp` (`InitAllComponents` constructor)
4. Add test in `tests/test_<name>.cpp`
5. Re-run `cmake -B build ...` to pick up new files

## Project Structure

```
digital_circuit_simulation/
├── include/dcs/
│   ├── Circuit.h, Component.h, Net.h, Pin.h
│   ├── CircuitLibrary.h, ComponentFactory.h
│   └── components/          # 40+ component headers
├── src/
│   ├── Circuit.cpp, ComponentFactory.cpp, CircuitLibrary.cpp
│   └── components/          # Component implementations
├── tools/
│   └── dcs_tool.cpp         # CLI: list-types, JSON→C
├── tests/                   # gtest suite
├── shim/                    # C headers for JIT compilation
├── main.cpp                 # Simulation runner
└── CMakeLists.txt
```

## License

MIT