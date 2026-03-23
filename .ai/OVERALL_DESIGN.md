# Pipelined Systolic Matrix-Multiply Accelerator

## Overview

A parameterized systolic-array matrix-multiply accelerator in SystemVerilog. The design implements a weight-stationary dataflow across an NxN grid of pipelined multiply-accumulate (MAC) units, with a control FSM and scratchpad memory subsystem for tiled matrix multiplication.

The accelerator is synthesizable and includes a full Verilator-based verification flow with C++ testbenches, Python reference model, and Yosys synthesis scripts for area/timing/power analysis across configurations.

## Repository structure

```
systolic-accel/
├── README.md
├── Makefile
├── rtl/
│   ├── mac_unit.sv
│   ├── systolic_array.sv
│   ├── controller.sv
│   ├── scratchpad.sv
│   └── top.sv
├── tb/
│   ├── tb_mac_unit.cpp
│   ├── tb_systolic_array.cpp
│   ├── tb_top.cpp
│   └── ref_matmul.py
├── synth/
│   ├── yosys_synth.tcl
│   └── analyze.py
├── docs/
│   └── ARCHITECTURE.md
└── waves/
```

## Toolchain

### Required

- **Verilator 5.x+** — SystemVerilog to C++ compiler/simulator
- **GTKWave** — waveform viewer
- **Yosys** — open-source synthesis
- **Python 3.8+** with `numpy` and `matplotlib`

```bash
# Ubuntu/Debian
sudo apt-get install verilator gtkwave yosys
pip install numpy matplotlib

# macOS
brew install verilator yosys
brew install --cask gtkwave
pip install numpy matplotlib
```

If the package manager provides Verilator 4.x, build from source:

```bash
git clone https://github.com/verilator/verilator
cd verilator && autoconf && ./configure && make -j$(nproc) && sudo make install
```

### Optional

**Xilinx Vivado WebPACK** — for FPGA synthesis with realistic timing and power reports. Not required; Yosys is sufficient for relative comparisons.

### Bootstrap

```bash
mkdir systolic-accel && cd systolic-accel
git init
mkdir -p rtl tb synth docs waves
echo -e "waves/\nobj_dir/\n*.vcd" > .gitignore
```

## SystemVerilog conventions

- Use `always_ff` and `always_comb` exclusively (never `always @`).
- Use `logic` type everywhere (never `reg` or `wire`).
- Active-low reset (`rst_n`).
- Non-blocking assignments (`<=`) in sequential blocks, blocking (`=`) in combinational blocks.
- Comment what each `always_ff` block synthesizes to physically (e.g., "N flip-flops", "registered multiplier output").

## Milestones

### M1: MAC unit

A parameterized, pipelined multiply-accumulate unit.

**Module: `rtl/mac_unit.sv`**

Parameters:

- `DATA_WIDTH` — operand bit width (default 16)
- `ACC_WIDTH` — accumulator bit width (default 32)

Ports:


| Port        | Direction | Width      | Description                                      |
| ----------- | --------- | ---------- | ------------------------------------------------ |
| `clk`       | input     | 1          | Clock                                            |
| `rst_n`     | input     | 1          | Active-low reset                                 |
| `enable`    | input     | 1          | Pipeline enable                                  |
| `clear_acc` | input     | 1          | Reset accumulator to zero                        |
| `a`         | input     | DATA_WIDTH | Operand A (activation)                           |
| `b`         | input     | DATA_WIDTH | Operand B (weight)                               |
| `a_out`     | output    | DATA_WIDTH | Registered passthrough of A (to right neighbor)  |
| `b_out`     | output    | DATA_WIDTH | Registered passthrough of B (to bottom neighbor) |
| `result`    | output    | ACC_WIDTH  | Accumulated result                               |


Pipeline stages:

1. **Stage 1 (multiply):** Register `a * b` into `mult_reg`. Simultaneously register `a` → `a_out` and `b` → `b_out`.
2. **Stage 2 (accumulate):** `acc_reg <= clear_acc ? mult_reg : acc_reg + mult_reg`

Total latency from input to reflected accumulator value: 2 cycles.

**Testbench: `tb/tb_mac_unit.cpp`**

Test cases:

- Basic accumulation: feed 4 (a, b) pairs, verify sum-of-products after pipeline drain
- Reset: assert `rst_n` low, verify accumulator clears to zero
- Clear mid-stream: assert `clear_acc` between two accumulation sequences, verify isolation
- Passthrough timing: verify `a_out` and `b_out` appear exactly 1 cycle after `a` and `b`
- Boundary: feed max-value inputs, verify accumulator overflow behavior at ACC_WIDTH boundary

Dump VCD to `waves/mac_unit.vcd`.

**Makefile target:**

```makefile
sim_mac:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim
	./obj_dir/mac_sim
```

**Done when:** All test cases pass. Waveform shows the 2-stage pipeline timing — `mult_reg` updates one cycle after inputs, `acc_reg` updates one cycle after that.

---

### M2: Systolic array

An NxN grid of MAC units with weight-stationary systolic dataflow.

**Module: `rtl/systolic_array.sv`**

Parameters:

- `ROWS`, `COLS` — array dimensions (default 4)
- `DATA_WIDTH`, `ACC_WIDTH` — forwarded to MAC units

Ports:


| Port                     | Direction | Width           | Description                 |
| ------------------------ | --------- | --------------- | --------------------------- |
| `clk`, `rst_n`, `enable` | input     | 1               | Global control              |
| `clear_acc`              | input     | 1               | Clear all accumulators      |
| `a_in[ROWS]`             | input     | DATA_WIDTH each | Left-edge activation inputs |
| `b_in[COLS]`             | input     | DATA_WIDTH each | Top-edge weight inputs      |
| `result[ROWS][COLS]`     | output    | ACC_WIDTH each  | Per-MAC accumulated results |


Internal wiring (generate block):

- `mac[i][j].a` ← `mac[i][j-1].a_out` (or `a_in[i]` for j=0)
- `mac[i][j].b` ← `mac[i-1][j].b_out` (or `b_in[j]` for i=0)

**Input skewing:**

For a correct matrix multiply C = A × B, row `i`'s activation stream must be delayed by `i` cycles and column `j`'s weight stream must be delayed by `j` cycles. Implement as shift-register chains on the array inputs:

```
Row 0: a_in[0] feeds directly
Row 1: a_in[1] feeds through 1 register delay
Row 2: a_in[2] feeds through 2 register delays
Row 3: a_in[3] feeds through 3 register delays
(Same pattern for columns with b_in)
```

Implement as a separate `input_skew` module or inline in `systolic_array.sv`.

**Timing:**

For an N×N matmul on an N×N array with 2-stage MAC pipeline:

- First valid output: cycle `N + N + 1` (max skew + pipeline depth)
- Last valid output: cycle `N + N + N` (last skewed input propagates through full array)
- All N² results are valid after the drain completes

**Reference model: `tb/ref_matmul.py`**

```python
import numpy as np
import json
import sys

def generate(n=4, seed=42):
    rng = np.random.RandomState(seed)
    A = rng.randint(-128, 127, (n, n)).astype(np.int16)
    B = rng.randint(-128, 127, (n, n)).astype(np.int16)
    C = A.astype(np.int32) @ B.astype(np.int32)
    json.dump({"A": A.tolist(), "B": B.tolist(), "C": C.tolist()},
              open("tb/test_vectors.json", "w"))

if __name__ == "__main__":
    generate(int(sys.argv[1]) if len(sys.argv) > 1 else 4)
```

**Testbench: `tb/tb_systolic_array.cpp`**

- Load test vectors from `tb/test_vectors.json` (or hardcode small cases initially)
- Feed the pre-skewed input sequence cycle-by-cycle
- Wait for full drain period
- Read `result[i][j]` for all i, j and compare against reference `C[i][j]`
- Test cases: identity multiply, random matrices, zero matrix, matrix with single nonzero element (verifies routing)
- Dump VCD to `waves/systolic_array.vcd`

**Done when:** 4×4 matmul produces correct results for all test cases. Can trace a single partial product (e.g., A[1][2] × B[2][3]) through the waveform and verify it arrives at MAC[1][3] on the expected cycle.

---

### M3: Synthesis and design-space exploration

Synthesize the array across configurations and measure area, critical path, and throughput.

**Synthesis script: `synth/yosys_synth.tcl`**

```tcl
read_verilog -sv rtl/mac_unit.sv rtl/systolic_array.sv
synth -top systolic_array
stat
```

Parameter sweeps via command line:

```bash
yosys -p "read_verilog -sv rtl/mac_unit.sv rtl/systolic_array.sv; \
          chparam -set DATA_WIDTH 8 systolic_array; \
          synth -top systolic_array; stat"
```

**Experiments:**

**Experiment 1 — Data width:** Synthesize 4×4 array at INT8, INT16, INT32. Record cell count. Expected: roughly quadratic scaling with data width (multiplier area ∝ width²).

**Experiment 2 — Array size:** Synthesize at 2×2, 4×4, 8×8, 16×16 with INT8. Record cell count. Expected: quadratic scaling with N (linear in MAC count).

**Experiment 3 — Pipeline depth:** Add a `PIPELINE_DEPTH` parameter to `mac_unit.sv`:

- `PIPELINE_DEPTH=1`: Combinational multiply + accumulate, no intermediate register
- `PIPELINE_DEPTH=2`: Register after multiply (default from M1)
- `PIPELINE_DEPTH=3`: Register after multiply, register after add

Synthesize 4×4 INT16 at each depth. Record cell count and critical path. Compute effective throughput: `freq × N² / total_cycles` where `freq = 1 / critical_path` and `total_cycles = 2N - 1 + pipeline_depth - 1`.

**Analysis script: `synth/analyze.py`**

Parse Yosys `stat` output for each configuration. Generate bar charts:

- Cell count vs. data width (4×4 array)
- Cell count vs. array size (INT8)
- Critical path vs. pipeline depth (4×4 INT16)
- Effective throughput vs. pipeline depth (4×4 INT16)

Output charts to `docs/`.

**Done when:** Charts generated and show expected trends. INT8→INT16 area ratio is roughly 4×. Pipeline depth tradeoff is quantified.

---

### M4 (stretch): Controller and scratchpad

Memory subsystem and control FSM for tiled matrix multiplication.

**Module: `rtl/scratchpad.sv`**

Parameters:

- `DEPTH` — number of entries
- `WIDTH` — bits per entry

Ports:


| Port    | Direction | Width         | Description                 |
| ------- | --------- | ------------- | --------------------------- |
| `clk`   | input     | 1             | Clock                       |
| `we`    | input     | 1             | Write enable                |
| `addr`  | input     | $clog2(DEPTH) | Address                     |
| `wdata` | input     | WIDTH         | Write data                  |
| `rdata` | output    | WIDTH         | Read data (1-cycle latency) |


Implementation: synchronous read/write register array.

**Module: `rtl/controller.sv`**

FSM states: `IDLE` → `LOAD_TILE` → `FEED_ARRAY` → `DRAIN_ARRAY` → `STORE_RESULT` → (next tile or `DONE`).

Inputs:

- `start` — begin computation
- Matrix dimensions M, K, N (must be multiples of array size)

Outputs:

- Scratchpad addresses and read/write enables for A, B, C
- Array control signals (`enable`, `clear_acc`)
- `done` — computation complete

Start with the non-tiled case (M=N=K=array_size). Then generalize to arbitrary multiples with a tiling loop over K (accumulating partial results across tiles).

**Module: `rtl/top.sv`**

Wire together: scratchpad A (M×K), scratchpad B (K×N), scratchpad C (M×N), systolic array, controller.

**Testbench: `tb/tb_top.cpp`**

- Pre-load scratchpads A and B via direct memory writes
- Assert `start`, poll `done`
- Read scratchpad C and verify against reference
- Test cases: single-tile 4×4, multi-tile 8×8 on 4×4 array, rectangular 4×8 × 8×4

**Done when:** Tiled 8×8 matmul on a 4×4 array produces correct results. Controller correctly accumulates partial sums across K-dimension tiles.

---

### M5 (stretch): Analysis writeup

Document findings in `docs/ARCHITECTURE.md`:

- Measured area scaling vs. data width and array size — comparison to theoretical expectations
- Pipeline depth tradeoff — where does deeper pipelining stop improving throughput?
- Memory bandwidth requirement: given the array's peak throughput in MACs/cycle, how many bytes/cycle must the scratchpad deliver to keep the array fully utilized? Compare to published numbers for real accelerators (H100: ~2 bytes/FLOP, WSE-3: much higher).
- Back-of-envelope: 900,000 copies of the MAC array on a 46,000mm² wafer — aggregate peak throughput and SRAM requirement to sustain it.

