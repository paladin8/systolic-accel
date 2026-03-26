# Systolic Matrix-Multiply Accelerator

A parameterized systolic-array matrix-multiply accelerator in SystemVerilog. Implements a weight-stationary dataflow across an NxN grid of pipelined multiply-accumulate (MAC) units, with a control FSM and scratchpad memory subsystem for tiled matrix multiplication.

## Prerequisites

- [Verilator](https://www.veripool.org/verilator/) 5.x+
- [Yosys](https://yosyshq.net/yosys/)
- [GTKWave](https://gtkwave.sourceforge.net/)
- [uv](https://docs.astral.sh/uv/) (Python package manager)

```bash
# Ubuntu/Debian
sudo apt-get install verilator gtkwave yosys

# Python dependencies
uv sync
```

## Usage

```bash
make sim_mac       # Run MAC unit testbench
make sim_array     # Run systolic array testbench
make sim_top       # Run full accelerator testbench
```

Waveforms are written to `waves/` and can be viewed with GTKWave.

## Project Structure

```
rtl/          SystemVerilog source
tb/           C++ testbenches (Verilator) and Python reference model
synth/        Yosys synthesis scripts and analysis
docs/         Architecture documentation
waves/        VCD waveform output (gitignored)
```

## Design Overview

- **NxN systolic array** of pipelined MAC units (default 4x4, 16-bit operands, 32-bit accumulator)
- **Weight-stationary dataflow (TPU-style)** — weights pre-loaded, activations stream through, partial sums flow down
- **2-stage MAC pipeline** — multiply activation by stored weight, then add partial sum from above
- **Scratchpad memory** and **tiling controller** for matrices larger than the array

See `docs/ARCHITECTURE.md` for detailed design analysis.
