# Milestone 2: Systolic Array — Design Spec & Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an NxN weight-stationary systolic array (TPU-style) that computes matrix multiplication C = A x B.

**Architecture:** Weights are pre-loaded into each MAC and stay fixed during compute. Activations stream left-to-right through each row. Partial sums flow top-to-bottom through each column, accumulating one product per MAC. Results emerge from the bottom edge. Inline shift-register chains skew row `k` by `k` cycles so that activations arrive at the correct time for the partial sum chain.

**Tech Stack:** SystemVerilog (Verilator 5.020), C++ testbench, Python reference model (numpy), GNU Make

**Spec reference:** `.ai/OVERALL_DESIGN.md` M2 section

---

## Design Decisions

1. **Weight-stationary dataflow (TPU-style)** — Weights pre-loaded into MACs, activations stream through, partial sums flow down. Matches Google TPU v1 architecture.

2. **Input skewing: inline, activations only** — Row `k` gets `k` shift registers. No column skewing — weights are pre-loaded, not streamed.

3. **Drain output** — Results emerge from `drain_out[0:COLS-1]` (bottom row `psum_out`). No internal result storage — the testbench (or M4 controller) captures results at the correct cycle.

4. **Weight loading via shift chain** — Weights shift down through each column via `b_out` chains. Feed B rows in reverse order (B[K-1] first, B[0] last). After K cycles, MAC[k][j] holds B[k][j].

5. **Test vectors: plain text** — Python reference model writes a simple text file. Parsed with `fscanf` in C++.

6. **Test coverage: thorough + timing** — 7 hardcoded tests plus 1 Python-generated random test.

7. **Skew registers: freeze on enable=0** — Skew registers gated by `enable` and cleared by `rst_n`, matching MAC behavior.

---

## Module Interface

### `rtl/systolic_array.sv`

Parameters:
- `ROWS` — array row count (default 4)
- `COLS` — array column count (default 4)
- `DATA_WIDTH` — operand width (default 16)
- `ACC_WIDTH` — accumulator width (default 32)

Ports:

| Port | Direction | Width | Description |
|------|-----------|-------|-------------|
| `clk` | input | 1 | Clock |
| `rst_n` | input | 1 | Active-low reset |
| `enable` | input | 1 | Pipeline enable (freezes all registers when low) |
| `load_weight` | input | 1 | Weight loading mode |
| `a_in[0:ROWS-1]` | input | DATA_WIDTH each | Left-edge activation inputs |
| `b_in[0:COLS-1]` | input | DATA_WIDTH each | Top-edge weight inputs (used during loading) |
| `drain_out[0:COLS-1]` | output | ACC_WIDTH each | Bottom-edge partial sum outputs |

### Internal Structure

**Skew registers (inline generate block, activations only):**
- Row `k`: chain of `k` registers delaying `a_in[k]` before it reaches MAC[k][0]
- Row 0: direct wiring (no registers)
- All registers gated by `enable`, cleared by `rst_n`
- Total: `N*(N-1)/2` registers of DATA_WIDTH bits

**MAC grid (generate block):**
- Activations (horizontal): `mac[k][j].a` ← `mac[k][j-1].a_out` (or skewed `a_in[k]` for j=0)
- Weights (vertical, for loading): `mac[k][j].b` ← `mac[k-1][j].b_out` (or `b_in[j]` for k=0)
- Partial sums (vertical): `mac[k][j].psum_in` ← `mac[k-1][j].psum_out` (or `0` for k=0)
- All MACs share `clk`, `rst_n`, `enable`, `load_weight`
- `drain_out[j]` ← `mac[ROWS-1][j].psum_out`

### Operation Phases

**Weight loading** (`load_weight=1`, K cycles):
Feed B rows in reverse order through `b_in`. Weights shift down via `b_out` chain.
- Cycle 0: `b_in[j] = B[K-1][j]` for all j
- Cycle 1: `b_in[j] = B[K-2][j]`
- ...
- Cycle K-1: `b_in[j] = B[0][j]`

After K cycles, MAC[k][j] holds B[k][j].

**Compute** (`load_weight=0`):
Feed activations through `a_in`. Each cycle `m` (m=0..M-1): `a_in[k] = A[m][k]` for all k. Hardware skew delays row `k` by `k` cycles. Partial sums chain down each column. Results emerge at `drain_out`.

### Timing (N=4, square matmul C = A x B)

- Weight loading: N cycles
- Compute start: cycle 0 (after weight loading)
- `C[m][j]` valid at `drain_out[j]` at compute cycle `N + m + j`
  - Derivation: activation A[m][k] arrives at MAC[k][j] at cycle `m + k + j` (m feed + k skew + j hops). Stage 1 at posedge `m+k+j`, stage 2 at posedge `m+k+j+1`. For MAC[N-1][j]: posedge `m+(N-1)+j+1 = m+N+j`.
- First valid result C[0][0]: compute cycle N = 4
- Last valid result C[N-1][N-1]: compute cycle N + 2(N-1) = 3N-2 = 10
- Total cycles (load + compute): N + (3N-2) = 4N-2 = 14

Results stream — each `drain_out[j]` changes every cycle. Must capture at the exact correct cycle.

### Register Budget (N=4, DATA_WIDTH=16, ACC_WIDTH=32)

- Skew registers: N*(N-1)/2 = 6 registers x 16 bits = 96 bits
- MAC registers: 16 MACs x (16 weight + 32 mult + 16 a_out + 16 b_out + 32 psum_out) bits = 16 x 112 = 1,792 bits
- Total: 1,888 bits

---

## Testing

### Reference Model: `tb/ref_matmul.py`

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

### Testbench: `tb/tb_systolic_array.cpp`

**Helpers:**
- `tick()` — one clock cycle (same pattern as M1)
- `reset()` — 2 cycles low, 1 settle
- `CHECK()` macro — same as M1
- `load_weights(dut, tfp, B[N][N])` — feeds B rows in reverse order through b_in over N cycles
- `feed_and_capture(dut, tfp, A[N][N], result[N][N])` — feeds activations for N rows, captures drain_out at correct cycles into result array. Calls `reset()` is NOT included — caller must reset before weight loading.

**Input feeding:** The testbench feeds un-skewed activations — hardware handles skewing. Each compute cycle `m` (m=0..N-1): `a_in[k] = A[m][k]` for all k.

**Result capture:** `C[m][j]` is captured from `drain_out[j]` after compute tick `N+m+j`.

**Test cases:**

1. **Identity multiply** — A = I, B = known matrix. C = B.
2. **Single nonzero element** — A[1][2] = 5, B[2][3] = 7. Only C[1][3] = 35.
3. **Zero matrix** — A = arbitrary, B = 0. All results = 0.
4. **Counting matrix** — A[i][j] = i*N+j+1, B = I. C = A.
5. **Negative values** — Mixed positive/negative, hardcoded expected values.
6. **Weight reuse** — Load weights once, compute C1 = A1*B and C2 = A2*B back-to-back without reloading. Verifies the core weight-stationary advantage.
7. **Timing verification** — Verify C[0][0] valid at compute cycle N, C[N-1][N-1] valid at cycle 3N-2.
8. **Random (file-loaded)** — Load `tb/test_vectors.txt`, compare against reference C.

### Makefile Target

```makefile
sim_array:
	uv run python tb/ref_matmul.py 4
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		--top-module systolic_array \
		rtl/mac_unit.sv rtl/systolic_array.sv tb/tb_systolic_array.cpp \
		-o array_sim
	./obj_dir/array_sim
```

VCD output: `waves/systolic_array.vcd`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `Makefile` | Modify | Add `sim_array` target |
| `rtl/systolic_array.sv` | Create | NxN MAC grid with weight loading and activation skewing |
| `tb/ref_matmul.py` | Create | Python reference model, plain-text output |
| `tb/tb_systolic_array.cpp` | Create | Verilator C++ testbench — 7 hardcoded + 1 random test |

---

## Implementation Tasks

### Task 1: Reference Model

**Files:**
- Create: `tb/ref_matmul.py`

- [ ] **Step 1: Write `tb/ref_matmul.py`**

See reference model code above.

- [ ] **Step 2: Test it**

Run: `uv run python tb/ref_matmul.py 4`
Expected: `tb/test_vectors.txt` created with 4, then 4 rows of A, 4 rows of B, 4 rows of C.

---

### Task 2: Makefile Update

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add `sim_array` target**

Add `sim_array` to `.PHONY` and add the target.

---

### Task 3: Systolic Array RTL

**Files:**
- Create: `rtl/systolic_array.sv`

- [ ] **Step 1: Write `rtl/systolic_array.sv`**

Module structure:
1. Parameter declarations (ROWS, COLS, DATA_WIDTH, ACC_WIDTH)
2. Port declarations (clk, rst_n, enable, load_weight, a_in, b_in, drain_out)
3. Internal wire declarations for MAC interconnect (a_wire, b_wire, psum_wire)
4. Generate block: row skew shift registers (row k gets k registers, activations only)
5. Generate block: NxN MAC instantiation with wiring:
   - `mac[k][j].a` ← skewed a (j=0) or `mac[k][j-1].a_out`
   - `mac[k][j].b` ← `b_in[j]` (k=0) or `mac[k-1][j].b_out`
   - `mac[k][j].psum_in` ← `0` (k=0) or `mac[k-1][j].psum_out`
   - `drain_out[j]` ← `mac[ROWS-1][j].psum_out`

Key implementation details:
- Skew registers use `always_ff @(posedge clk or negedge rst_n)` with enable gating
- For row k (k>0): `a_skew[k][0] <= a_in[k]`, `a_skew[k][s] <= a_skew[k][s-1]`
- Row 0: wire directly (no skew registers)
- psum_in for top row (k=0) is constant `'0`
- All MACs share `load_weight` signal

- [ ] **Step 2: Verify it compiles**

Run: `verilator --cc --trace -Wall -Wno-fatal rtl/mac_unit.sv rtl/systolic_array.sv`
Expected: Clean compilation.

---

### Task 4: Testbench Scaffold + Identity Test

**Files:**
- Create: `tb/tb_systolic_array.cpp`

- [ ] **Step 1: Write testbench scaffold**

Scaffold includes:
- Verilator includes and `Vsystolic_array.h`
- Constants: `N=4`
- `sim_time`, `test_failures`, `CHECK()` macro (same as M1)
- `tick()`, `reset()` helpers
- `load_weights(dut, tfp, B[N][N])` — N ticks feeding B rows in reverse order
- `feed_and_capture(dut, tfp, A[N][N], result[N][N])` — feeds activations, captures drain_out at correct cycles
- `check_results(result[N][N], expected[N][N], test_name)` — compares captured vs expected
- Identity multiply test: A = I, B = known matrix, verify result = B
- `main()` with VCD setup, test call, reporting

- [ ] **Step 2: Build and run**

Run: `make sim_array`
Expected: Identity test passes.

---

### Task 5: Single-Element and Zero Tests

**Files:**
- Modify: `tb/tb_systolic_array.cpp`

- [ ] **Step 1: Add single nonzero element test**

A[1][2] = 5, all else 0. B[2][3] = 7, all else 0. Only result[1][3] should be 35.

- [ ] **Step 2: Add zero matrix test**

A = arbitrary, B = 0. All results should be 0.

- [ ] **Step 3: Build and run**

Run: `make sim_array`
Expected: All 3 tests pass.

---

### Task 6: Counting Matrix and Negative Values Tests

**Files:**
- Modify: `tb/tb_systolic_array.cpp`

- [ ] **Step 1: Add counting matrix test**

A[i][j] = i*4+j+1, B = I. Result should equal A.

- [ ] **Step 2: Add negative values test**

Hardcoded small matrices with mixed positive/negative entries and pre-computed expected results.

- [ ] **Step 3: Build and run**

Run: `make sim_array`
Expected: All 5 tests pass.

---

### Task 7: Weight Reuse Test

**Files:**
- Modify: `tb/tb_systolic_array.cpp`

- [ ] **Step 1: Add weight reuse test**

Load weights B once. Compute C1 = A1 * B, then compute C2 = A2 * B back-to-back without reloading weights. Use two different A matrices with known expected results.

Key details:
- Call `load_weights` once
- Call `feed_and_capture` for A1, verify C1
- Call `feed_and_capture` again for A2 (no reset, no reload), verify C2
- This requires `feed_and_capture` to work without a preceding reset — the pipeline drains naturally between computations since trailing zeros flush the partial sums

- [ ] **Step 2: Build and run**

Run: `make sim_array`
Expected: All 6 tests pass.

---

### Task 8: Timing Verification Test

**Files:**
- Modify: `tb/tb_systolic_array.cpp`

- [ ] **Step 1: Add timing test**

Feed a known multiply. At specific compute cycle counts:
- At compute cycle N (=4): C[0][0] should be valid at drain_out[0]
- Before compute cycle 3N-2 (=10): C[N-1][N-1] should NOT yet be valid
- At compute cycle 3N-2 (=10): C[N-1][N-1] should be valid at drain_out[N-1]

This requires a manual tick loop that checks drain_out at intermediate cycles.

- [ ] **Step 2: Build and run**

Run: `make sim_array`
Expected: All 7 tests pass.

---

### Task 9: Random Test (Python-Generated)

**Files:**
- Modify: `tb/tb_systolic_array.cpp`

- [ ] **Step 1: Add file-loading test**

Parse `tb/test_vectors.txt`:
- Read N from first line
- Read N rows of A, N rows of B, N rows of C
- Load weights B, feed activations A, capture results
- Compare against expected C

- [ ] **Step 2: Build and run**

Run: `make sim_array`
Expected: All 8 tests pass (7 hardcoded + 1 file-loaded random).

---

### Task 10: Final Verification

- [ ] **Step 1: Clean build and run**

```bash
make clean && make sim_array
```

Expected: All tests pass, `waves/systolic_array.vcd` created.

- [ ] **Step 2: Verify M1 still passes**

```bash
make sim_mac
```

Expected: All 7 M1 tests still pass.

Milestone 2 is complete when all tests pass for both `sim_mac` and `sim_array`.
