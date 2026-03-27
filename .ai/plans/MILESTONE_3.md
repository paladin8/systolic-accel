# Milestone 3: Synthesis & Design-Space Exploration — Design Spec & Implementation Plan

**Goal:** Add a configurable pipeline depth to the MAC unit, synthesize with Yosys, and run design-space experiments (data width, array size, pipeline depth) targeting iCE40 to measure area and throughput tradeoffs.

**Architecture:** The MAC unit gains a `PIPELINE_DEPTH` parameter (1, 2, or 3 stages). `systolic_array.sv` uses packed ports for Yosys compatibility. A Python experiment driver invokes Yosys across parameter combinations, and an analysis script produces terminal tables and PNG charts.

**Tech Stack:** SystemVerilog, Yosys 0.33 (iCE40 target), Python (numpy, matplotlib), Verilator 5.020 for testing, GNU Make

**Spec reference:** `.ai/OVERALL_DESIGN.md` M3 section

---

## Design Decisions

1. **Configurable pipeline depth** — `PIPELINE_DEPTH` parameter added to `mac_unit.sv` with default 2 (backwards compatible). Depth 1 = combinational multiply-add, depth 2 = current design, depth 3 = extra register after add. Forwarded through `systolic_array.sv`.

2. **Packed ports on systolic_array.sv** — `systolic_array.sv` uses packed bit-vector ports (`a_in`, `b_in`, `drain_out`) instead of unpacked arrays. Yosys 0.33 cannot parse unpacked array port declarations. Internal wiring still uses unpacked arrays (which Yosys handles fine). This means one module serves both simulation and synthesis — no separate wrapper needed.

3. **iCE40 target** — `synth_ice40` gives real LUT/FF counts in one pass. No hard multiplier blocks on iCE40, so all logic maps to LUTs — making area comparisons clean. Absolute numbers are iCE40-specific but relative trends are architecture-independent.

4. **Three experiments** (ACC_WIDTH = 2 × DATA_WIDTH for fair comparison across widths):
   - Experiment 1 (data width): 4×4 array, DATA_WIDTH ∈ {8, 16, 32}, ACC_WIDTH ∈ {16, 32, 64}, depth 2
   - Experiment 2 (array size): ROWS=COLS ∈ {2, 4, 8, 16}, DATA_WIDTH=8, ACC_WIDTH=16, depth 2
   - Experiment 3 (pipeline depth): 4×4, DATA_WIDTH=16, ACC_WIDTH=32, PIPELINE_DEPTH ∈ {1, 2, 3}

5. **Output: tables + charts** — Terminal tables for quick feedback, PNG charts saved to `docs/` for M5 writeup.

6. **Depth 3 limitation** — PIPELINE_DEPTH > 2 breaks the systolic column chain. MAC[k]'s `psum_out` takes 3 cycles but MAC[k+1]'s activation arrives only 1 cycle later (row stagger = 1). The add stage reads stale `psum_in`. Depth 3 is tested at the MAC level and synthesized for area comparison, but cannot be simulated correctly at the array level. Supporting it would require doubling the skew registers (2 per row instead of 1).

---

## RTL Changes

### `mac_unit.sv` — Pipeline Depth Parameter

New parameter: `PIPELINE_DEPTH` (default 2).

**Depth 1 (combinational multiply-add):**
- No `mult_reg`. Product and partial sum addition in one combinational path.
- `psum_out <= psum_in + ($signed(a) * $signed(weight_reg))`
- `a_out` and `b_out` still registered (needed for systolic flow).
- Systolic flow correctness: at depth 1, both `psum_out` and `a_out` are registered in a single cycle. MAC[k-1]'s `psum_out` and MAC[k]'s activation arrive at the same cycle boundary, so the column chain works correctly.

**Depth 2 (current design, no changes):**
- Stage 1: `mult_reg <= $signed(a) * $signed(weight_reg)`, register `a_out`, `b_out`
- Stage 2: `psum_out <= psum_in + mult_reg`

**Depth 3 (extra register after add):**
- Stage 1: same as depth 2
- Stage 2: `add_reg <= psum_in + mult_reg` (new intermediate register)
- Stage 3: `psum_out <= add_reg`

Weight register and passthrough registers (`a_out`, `b_out`) are unchanged across all depths.

### `systolic_array.sv` — Packed Ports + PIPELINE_DEPTH

Ports use packed bit vectors for Yosys compatibility:
- `input logic [ROWS*DATA_WIDTH-1:0] a_in`
- `input logic [COLS*DATA_WIDTH-1:0] b_in`
- `output logic [COLS*ACC_WIDTH-1:0] drain_out`

Internally: unpacked arrays, skew registers, NxN MAC grid — same logic as M2 but with packed port slicing (`a_in[k*DATA_WIDTH +: DATA_WIDTH]`). Forwards `PIPELINE_DEPTH` to all MAC instantiations.

---

## Testing

### Pipeline Depth Testing

Since Verilator bakes parameters in at compile time (via `-GPIPELINE_DEPTH=N`), the testbench uses a compile-time constant: `#ifndef PIPELINE_DEPTH` / `#define PIPELINE_DEPTH 2` / `#endif`. Makefile targets pass `-CFLAGS "-DPIPELINE_DEPTH=N"` to match.

**MAC testbench (`tb/tb_mac_unit.cpp`):**
- All existing tests adapt to pipeline depth via `tick_n(dut, tfp, PIPELINE_DEPTH)` — result appears `PIPELINE_DEPTH` cycles after activation input
- Passthrough tests (`a_out`, `b_out`) remain at 1-cycle latency (depth-independent)
- Verified at depths 1, 2, and 3 via separate Makefile targets

**Array testbench (`tb/tb_systolic_array.cpp`):**
- Uses `pack_row()` and `extract_drain()` helpers for packed ports
- Timing formula: `C[m][j]` at `drain_out[j]` at compute cycle `N + PIPELINE_DEPTH - 2 + m + j`
- Derivation: activation reaches MAC[k][j] at cycle m+k+j. Psum propagates 1 cycle per row (limited by activation stagger, not MAC depth). Column of N MACs adds N-1 cycles. Result: `m + j + N + PIPELINE_DEPTH - 2`.
- For depth 2: `N + m + j` (matches M2). For depth 1: `N - 1 + m + j`.
- Verified at depths 1 and 2 (depth 3 is MAC-level and synthesis only)

**Makefile targets:**
```makefile
sim_mac_d1:   # -GPIPELINE_DEPTH=1 --Mdir obj_dir_mac_d1
sim_mac_d3:   # -GPIPELINE_DEPTH=3 --Mdir obj_dir_mac_d3
sim_array_d1: # -GPIPELINE_DEPTH=1 --Mdir obj_dir_array_d1
```

Separate `--Mdir` directories per target to avoid build artifact collisions.

### Synthesis Verification

After running experiments, verify:
- INT8 → INT16 area ratio ≈ 4× (multiplier area ∝ width²)
- Array size scaling ≈ quadratic in N (linear in MAC count)
- Deeper pipeline = more FFs, area tradeoffs vary
- Throughput comparison requires actual timing data (see future work: place and route)

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `rtl/mac_unit.sv` | Modify | Add `PIPELINE_DEPTH` parameter with depth 1/2/3 support |
| `rtl/systolic_array.sv` | Modify | Packed ports, forward `PIPELINE_DEPTH` to MAC instantiations |
| `tb/tb_mac_unit.cpp` | Modify | Pipeline-depth-aware timing in all tests |
| `tb/tb_systolic_array.cpp` | Modify | Pipeline-depth-aware timing, packed port helpers |
| `synth/yosys_synth.tcl` | Create | Quick manual synthesis script (iCE40) |
| `synth/run_sweep.py` | Create | Experiment driver — invokes Yosys across configs |
| `synth/analyze.py` | Create | Parse results, print tables, generate PNG charts |
| `Makefile` | Modify | Add pipeline depth targets and synth target |
| `.gitignore` | Modify | Add `synth/logs/`, `synth/results.json`, `obj_dir_*/` |

---

## Implementation Tasks

### Task 1: Add PIPELINE_DEPTH to mac_unit.sv

**Files:**
- Modify: `rtl/mac_unit.sv`

- [x] **Step 1: Add parameter and implement depth 1/2/3 logic**

Use `generate` with `if`/`else` blocks. Weight register and passthrough registers (`a_out`, `b_out`) in a shared `always_ff`, pipeline-depth-specific logic in generate branches.

- [x] **Step 2: Verify all three depths compile**

---

### Task 2: Forward PIPELINE_DEPTH in systolic_array.sv, convert to packed ports

**Files:**
- Modify: `rtl/systolic_array.sv`

- [x] **Step 1: Convert ports to packed bit vectors**

Replace unpacked array ports with packed: `a_in[ROWS*DATA_WIDTH-1:0]`, etc. Use `+:` slicing internally. Add `PIPELINE_DEPTH` parameter and forward to MAC instantiations.

- [x] **Step 2: Verify Verilator and Yosys both accept it**

---

### Task 3: Update MAC testbench for pipeline-depth-aware timing

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [x] **Step 1: Add compile-time PIPELINE_DEPTH constant and `tick_n()` helper**

- [x] **Step 2: Verify default depth 2 passes**

---

### Task 4: Add pipeline depth Makefile targets and verify depths 1/3

**Files:**
- Modify: `Makefile`

- [x] **Step 1: Add sim_mac_d1 and sim_mac_d3 targets**

Separate `--Mdir` directories (`obj_dir_mac_d1`, `obj_dir_mac_d3`). Pass `-CFLAGS "-DPIPELINE_DEPTH=N"`.

- [x] **Step 2: Run all three MAC targets — all pass**

---

### Task 5: Update array testbench for pipeline-depth-aware timing

**Files:**
- Modify: `tb/tb_systolic_array.cpp`
- Modify: `Makefile`

- [x] **Step 1: Add packed port helpers and generalized timing**

`pack_row()` packs N int16 values into uint64_t. `extract_drain()` extracts int32 from WData array. `DRAIN_BASE = N + PIPELINE_DEPTH - 2`.

- [x] **Step 2: Add sim_array_d1 Makefile target**

Depth 3 omitted (breaks column chain). Separate `--Mdir obj_dir_array_d1`.

- [x] **Step 3: Verify depths 1 and 2 pass all 8 tests**

---

### Task 6: Yosys synthesis script

**Files:**
- Create: `synth/yosys_synth.tcl`

- [x] **Step 1: Write Tcl script for quick manual synthesis**

Reads `rtl/mac_unit.sv` and `rtl/systolic_array.sv`, runs `synth_ice40 -top systolic_array`, prints `stat`. For parameter sweeps, `run_sweep.py` constructs inline Yosys commands with `chparam` directly.

- [x] **Step 2: Verify default synthesis succeeds**

---

### Task 7: Experiment driver

**Files:**
- Create: `synth/run_sweep.py`
- Modify: `.gitignore`

- [x] **Step 1: Write experiment driver**

Defines three experiment parameter sweeps. For each config: constructs Yosys command with `chparam`, runs via `subprocess`, parses `stat` output for LUT/FF counts, saves to `synth/results.json`.

- [x] **Step 2: Add synth artifacts to .gitignore**

- [x] **Step 3: Test with a single experiment**

---

### Task 8: Analysis script

**Files:**
- Create: `synth/analyze.py`

- [x] **Step 1: Write analysis script**

Reads `synth/results.json`. Prints ASCII tables to terminal. Generates PNG charts:
- `docs/area_vs_datawidth.png`
- `docs/area_vs_arraysize.png`
- `docs/area_vs_depth.png`
- `docs/throughput_vs_depth.png`

Cycle efficiency formula: `N² / compute_cycles` where `compute_cycles = (2N - 1) + (PIPELINE_DEPTH - 1)` (compute phase only). Note: this is MACs/cycle, not MACs/second — actual throughput requires timing data from place and route.

---

### Task 9: Run all experiments

- [x] **Step 1: Run full sweep**

- [x] **Step 2: Generate analysis**

- [x] **Step 3: Sanity-check results**

Results:
- INT8 → INT16 LUT ratio: 4.1× (expected ~4×)
- 2×2 → 4×4 LUT ratio: 4.1× (expected ~4×)
- Depth 1 has MORE LUTs than depth 2 (17060 vs 12727) — combinational path harder to optimize
- Depth 2 and 3 similar LUTs (~12700), depth 3 adds 512 FFs (16 MACs × 32-bit add_reg)

---

### Task 10: Makefile synth target and final verification

**Files:**
- Modify: `Makefile`

- [x] **Step 1: Add synth target**

- [x] **Step 2: Full clean verification — all targets pass**

---

Milestone 3 is complete when all simulation tests pass at depths 1 and 2, MAC tests pass at depth 3, synthesis runs across all configurations, and charts show expected area trends.
