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

A parameterized, pipelined multiply-accumulate unit for weight-stationary systolic dataflow. Each MAC holds a pre-loaded weight, multiplies it by streaming activations, and forwards partial sums vertically.

**Module: `rtl/mac_unit.sv`**

Parameters:

- `DATA_WIDTH` — operand bit width (default 16)
- `ACC_WIDTH` — accumulator/partial-sum bit width (default 32)

Ports:


| Port          | Direction | Width      | Description                                           |
| ------------- | --------- | ---------- | ----------------------------------------------------- |
| `clk`         | input     | 1          | Clock                                                 |
| `rst_n`       | input     | 1          | Active-low reset                                      |
| `enable`      | input     | 1          | Pipeline enable                                       |
| `load_weight` | input     | 1          | Latch `b` into weight register                        |
| `a`           | input     | DATA_WIDTH | Activation input (from left neighbor or array edge)   |
| `b`           | input     | DATA_WIDTH | Weight data input (from top, used during loading)     |
| `a_out`       | output    | DATA_WIDTH | Registered passthrough of A (to right neighbor)       |
| `b_out`       | output    | DATA_WIDTH | Registered passthrough of B (to bottom neighbor)      |
| `psum_in`     | input     | ACC_WIDTH  | Partial sum from top neighbor (0 for top row)         |
| `psum_out`    | output    | ACC_WIDTH  | Partial sum to bottom neighbor                        |


Internal registers:

- `weight_reg` — stored weight, loaded when `load_weight` is high, fixed during compute
- `mult_reg` — stage 1 multiply result
- `a_out` — activation passthrough register
- `b_out` — weight passthrough register (for loading chain)
- `psum_out` — stage 2 partial sum output register

Pipeline stages:

1. **Weight register:** Loads `b` into `weight_reg` when `load_weight && enable`. Holds value otherwise.
2. **Stage 1 (multiply + passthrough):** `mult_reg <= $signed(a) * $signed(weight_reg)`. Simultaneously register `a` → `a_out` and `b` → `b_out`.
3. **Stage 2 (partial sum):** `psum_out <= psum_in + mult_reg`

Total latency from activation input to partial sum output: 2 cycles.

**Testbench: `tb/tb_mac_unit.cpp`**

Test cases:

- Weight loading: load a weight via `load_weight`, verify it persists during compute
- Multiply + passthrough timing: verify `a_out` appears 1 cycle after `a`, `mult_reg` uses stored weight
- Partial sum chain: feed `psum_in` and activation, verify `psum_out = psum_in + a * weight`
- Weight loading chain: verify `b_out` passes weights through for column loading
- Reset: assert `rst_n` low, verify all registers clear
- Enable stall: verify all registers freeze when `enable=0`
- Signed values: negative activations and weights

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

**Done when:** All test cases pass. Waveform shows weight staying fixed during compute, 2-stage pipeline timing for partial sums, and correct activation passthrough.

---

### M2: Systolic array

An NxN grid of MAC units with weight-stationary systolic dataflow, matching the Google TPU architecture.

**Module: `rtl/systolic_array.sv`**

Parameters:

- `ROWS`, `COLS` — array dimensions (default 4)
- `DATA_WIDTH`, `ACC_WIDTH` — forwarded to MAC units

Ports:


| Port                     | Direction | Width           | Description                                      |
| ------------------------ | --------- | --------------- | ------------------------------------------------ |
| `clk`, `rst_n`, `enable` | input     | 1               | Global control                                   |
| `load_weight`            | input     | 1               | Weight loading mode                              |
| `a_in[ROWS]`             | input     | DATA_WIDTH each | Left-edge activation inputs                      |
| `b_in[COLS]`             | input     | DATA_WIDTH each | Top-edge weight inputs (used during loading)     |
| `drain_out[COLS]`        | output    | ACC_WIDTH each  | Bottom-edge partial sum outputs (result stream)  |


**Operation phases:**

1. **Weight loading** (`load_weight=1`): Feed weight matrix B through `b_in`, one row per cycle in reverse row order (B[K-1] first, B[0] last). Weights shift down through each column via `b_out` chains. After K cycles, MAC[k][j] holds B[k][j].

2. **Compute** (`load_weight=0`): Feed activation rows through `a_in`. Activations flow left-to-right, partial sums flow top-to-bottom. Results emerge at `drain_out[j]` (bottom of each column).

Internal wiring (generate block):

- `mac[k][j].a` ← `mac[k][j-1].a_out` (or skewed `a_in[k]` for j=0)
- `mac[k][j].b` ← `mac[k-1][j].b_out` (or `b_in[j]` for k=0)
- `mac[k][j].psum_in` ← `mac[k-1][j].psum_out` (or `0` for k=0)
- `drain_out[j]` ← `mac[ROWS-1][j].psum_out`

**Input skewing (activations only):**

Row `k`'s activation is delayed by `k` cycles via inline shift-register chain. No column skewing — weights are pre-loaded, not streamed.

```
Row 0: a_in[0] feeds directly to MAC[0][0]
Row 1: a_in[1] feeds through 1 register delay
Row 2: a_in[2] feeds through 2 register delays
Row 3: a_in[3] feeds through 3 register delays
```

**Timing:**

For an N×N matmul (C = A × B) on an N×N array with 2-stage MAC pipeline:

- Weight loading: N cycles
- Compute: `C[m][j]` appears at `drain_out[j]` at cycle `N + m + j` from compute start
- First valid output (C[0][0]): cycle `N` from compute start
- All N² results streamed: cycle `N + (N-1) + (N-1)` = `3N - 2` from compute start
- Total cycles (load + compute): `N + 3N - 2` = `4N - 2`

Results stream from `drain_out` one at a time — the testbench (or M4 controller) must capture each `drain_out[j]` at the correct cycle.

**Reference model: `tb/ref_matmul.py`**

```python
import numpy as np
import sys

def generate(n=4, seed=42):
    rng = np.random.RandomState(seed)
    A = rng.randint(-128, 127, (n, n)).astype(np.int16)
    B = rng.randint(-128, 127, (n, n)).astype(np.int16)
    C = A.astype(np.int32) @ B.astype(np.int32)

    with open("tb/test_vectors.txt", "w") as f:
        f.write(f"{n}\n")
        for row in A:
            f.write(" ".join(str(x) for x in row) + "\n")
        for row in B:
            f.write(" ".join(str(x) for x in row) + "\n")
        for row in C:
            f.write(" ".join(str(x) for x in row) + "\n")

if __name__ == "__main__":
    generate(int(sys.argv[1]) if len(sys.argv) > 1 else 4,
             int(sys.argv[2]) if len(sys.argv) > 2 else 42)
```

**Testbench: `tb/tb_systolic_array.cpp`**

- Load weights via `load_weight` phase
- Feed activations (un-skewed — hardware handles skewing)
- Capture `drain_out[j]` at the correct cycle for each result element
- Compare against reference model
- Test cases: identity multiply, single nonzero element (verifies routing), zero matrix, counting matrix, negative values, timing verification, random (Python-generated)
- Dump VCD to `waves/systolic_array.vcd`

**Done when:** 4×4 matmul produces correct results for all test cases. Can trace a single partial product (e.g., A[1][2] × B[2][3]) through the waveform — weight B[2][3] loaded into MAC[2][3], activation A[1][2] enters row 2 and reaches column 3 via passthrough, partial sum emerges at `drain_out[3]`.

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
- Array control signals (`enable`, `load_weight`)
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

