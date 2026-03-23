# CLAUDE.md

## Project

Pipelined systolic-array matrix-multiply accelerator in SystemVerilog. Full spec in `.ai/OVERALL_DESIGN.md`.

## Milestones (in order)

1. **M1** — MAC unit (`rtl/mac_unit.sv`, `tb/tb_mac_unit.cpp`)
2. **M2** — NxN systolic array with input skewing (`rtl/systolic_array.sv`, `tb/tb_systolic_array.cpp`, `tb/ref_matmul.py`)
3. **M3** — Yosys synthesis & design-space exploration (`synth/yosys_synth.tcl`, `synth/analyze.py`)
4. **M4** — Controller + scratchpad for tiled matmul (`rtl/controller.sv`, `rtl/scratchpad.sv`, `rtl/top.sv`, `tb/tb_top.cpp`)
5. **M5** — Architecture analysis writeup (`docs/ARCHITECTURE.md`)

## SystemVerilog rules

- `always_ff` / `always_comb` only (never `always @`)
- `logic` everywhere (never `reg` / `wire`)
- Active-low reset: `rst_n`
- `<=` in sequential blocks, `=` in combinational blocks
- Comment what each `always_ff` synthesizes to (e.g. "N flip-flops")

## Toolchain

- **Verilator 5.020** for simulation (compile with `--cc --exe --build --trace -Wall -Wno-fatal`)
- **Yosys** for synthesis
- **Python** managed by **uv** — run Python scripts with `uv run python <script>`
- Dependencies (`numpy`, `matplotlib`) defined in `pyproject.toml`
- VCD waveforms go in `waves/`

## Build

All simulation targets use the Makefile. Pattern: `sim_mac`, `sim_array`, `sim_top`.

```bash
make sim_mac       # M1
make sim_array     # M2
make sim_top       # M4
```

## Key design details

- **Weight-stationary dataflow** — weights loaded via top-edge `b_in`, activations stream from left-edge `a_in`
- **2-stage MAC pipeline** — stage 1: multiply + register passthrough; stage 2: accumulate
- **Input skewing** — row `i` delayed by `i` cycles, column `j` delayed by `j` cycles
- **Timing** — first valid output at cycle `N + N + 1`, all results valid after cycle `3N`
- Default params: `DATA_WIDTH=16`, `ACC_WIDTH=32`, `ROWS=COLS=4`

## File conventions

- RTL in `rtl/`, testbenches in `tb/`, synthesis scripts in `synth/`
- Waveforms (`*.vcd`) and `obj_dir/` are gitignored
- Testbenches are C++ (Verilator), reference model is Python
