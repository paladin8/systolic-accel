# Milestone 4: Controller & Scratchpad for Tiled Matrix Multiplication

**Goal:** Add a memory subsystem (scratchpad SRAMs) and control FSM to perform tiled matrix multiplication autonomously. The host pre-loads matrices A and B, asserts `start`, and reads result matrix C when `done` is signaled. Supports full M/N/K tiling for matrices that are multiples of the array dimensions.

**Architecture:** Three vector-wide scratchpads (A, B, C) hold full matrices in pre-tiled layout. A controller FSM iterates over (mt, nt, kt) tile indices, driving the systolic array through weight-load / feed / drain / writeback phases per tile. A shared external port lets the host access scratchpads when the controller is idle. Drain results are captured into a register file inside the controller, then written back to the C scratchpad with read-modify-write accumulation for K-dimension tiling.

**Tech Stack:** SystemVerilog, Verilator 5.020 for simulation, Python (numpy) for reference model, GNU Make

**Spec reference:** `.ai/OVERALL_DESIGN.md` M4 section, builds on M1-M3 infrastructure

---

## Design Decisions

1. **Full M/N/K tiling** — The controller handles arbitrary matrix dimensions (M, K, N) as long as M is a multiple of ROWS, K is a multiple of ROWS, and N is a multiple of COLS. Three nested tile loops: mt (output rows), nt (output columns), kt (inner dimension). K-tiling requires partial sum accumulation via read-modify-write to the C scratchpad.

2. **Controller assumes PIPELINE_DEPTH=2** — The drain timing logic is designed for the default pipeline depth. PIPELINE_DEPTH=1 would cause drain outputs to overlap with the FEED phase, and PIPELINE_DEPTH=3 breaks the systolic column chain (see M3 plan). Supporting other depths would require adjusting the FSM timing.

3. **Vector-wide scratchpads** — Each scratchpad entry holds a full vector matching the array port width. SP_A is ROWS*DATA_WIDTH wide (one column of an A tile), SP_B is COLS*DATA_WIDTH wide (one row of a B tile), SP_C is COLS*ACC_WIDTH wide (one row of a C tile). One read per cycle feeds the array directly — no multi-cycle assembly needed.

4. **Shared external port, controller arbitrates** — A single external interface on `top.sv` with a 2-bit `ext_sel` signal (0=A, 1=B, 2=C). When the controller is IDLE, the external port drives scratchpad addresses and data. When the controller is running, it owns all scratchpad ports. No overlap, no arbitration logic — just a mux on controller state.

5. **Drain register file** — The controller captures drain_out values into a ROWS×COLS register file (ROWS*COLS*ACC_WIDTH bits total). This decouples the diagonal drain pattern from the sequential C scratchpad writes. After drain completes, the controller writes registers to SP_C row by row.

6. **Pre-tiled storage layout** — The host stores matrices in a tile-sequential format. The controller reads sequential addresses within each tile, making address computation simple (tile base + offset). The testbench handles the layout conversion.

---

## Modules

### `scratchpad.sv` — Generic Synchronous SRAM

Parameters:
- `DEPTH` — number of entries
- `WIDTH` — bits per entry

Ports:

| Port    | Direction | Width         | Description                 |
|---------|-----------|--------------|-----------------------------|
| `clk`   | input     | 1             | Clock                       |
| `we`    | input     | 1             | Write enable                |
| `addr`  | input     | $clog2(DEPTH) | Address                     |
| `wdata` | input     | WIDTH         | Write data                  |
| `rdata` | output    | WIDTH         | Read data (1-cycle latency) |

Implementation: `logic [WIDTH-1:0] mem [0:DEPTH-1]` array. Synchronous read and write in a single `always_ff` block. Read-first behavior (rdata reflects the value at addr before any write on the same cycle).

### `controller.sv` — Tiled Matmul FSM

Parameters:
- `ROWS`, `COLS` — array dimensions (forwarded from top)
- `DATA_WIDTH`, `ACC_WIDTH` — data widths
- `PIPELINE_DEPTH` — MAC pipeline depth (for drain timing)
- `SP_A_DEPTH`, `SP_B_DEPTH`, `SP_C_DEPTH` — scratchpad depths

Ports:

| Port | Direction | Width | Description |
|------|-----------|-------|-------------|
| `clk`, `rst_n` | input | 1 | Clock/reset |
| `start` | input | 1 | Begin computation |
| `done` | output | 1 | Computation complete |
| `M`, `K`, `N` | input | 16 | Matrix dimensions |
| `sp_a_addr` | output | $clog2(SP_A_DEPTH) | SP_A read address |
| `sp_a_rdata` | input | ROWS*DATA_WIDTH | SP_A read data |
| `sp_b_addr` | output | $clog2(SP_B_DEPTH) | SP_B read address |
| `sp_b_rdata` | input | COLS*DATA_WIDTH | SP_B read data |
| `sp_c_addr` | output | $clog2(SP_C_DEPTH) | SP_C address |
| `sp_c_we` | output | 1 | SP_C write enable |
| `sp_c_wdata` | output | COLS*ACC_WIDTH | SP_C write data |
| `sp_c_rdata` | input | COLS*ACC_WIDTH | SP_C read data |
| `arr_enable` | output | 1 | Array enable |
| `arr_load_weight` | output | 1 | Array weight load |
| `arr_a_in` | output | ROWS*DATA_WIDTH | Array activation input |
| `arr_b_in` | output | COLS*DATA_WIDTH | Array weight input |
| `arr_drain_out` | input | COLS*ACC_WIDTH | Array drain output |

**FSM states:** `IDLE` → `LOAD_WEIGHTS` → `FEED` → `DRAIN` → `WRITEBACK` → (next tile or `DONE`)

**Tiling loop:**

```
for mt = 0 .. M/ROWS - 1:
  for nt = 0 .. N/COLS - 1:
    for kt = 0 .. K/ROWS - 1:
      LOAD_WEIGHTS: read SP_B, drive b_in          (ROWS cycles)
      FEED:         read SP_A, drive a_in           (ROWS cycles)
      DRAIN:        capture drain_out into regs     (ROWS + COLS - 1 cycles)
      WRITEBACK:    write drain regs to SP_C        (ROWS or 2*ROWS cycles)
```

**Phase details:**

1. **LOAD_WEIGHTS** (ROWS cycles): Read SP_B in reverse row order within the tile (row ROWS-1 first, row 0 last). B tile (kt, nt) base address = `(kt * N_tiles + nt) * ROWS`. Drive `arr_b_in = sp_b_rdata`, assert `arr_load_weight`. Drive `arr_a_in = 0` — this also flushes the skew registers from any previous tile's computation (max skew depth = ROWS-1, so ROWS zero-cycles fully clears them).

2. **FEED** (ROWS cycles): Read SP_A sequentially within the tile (column 0 through ROWS-1). A tile (mt, kt) base address = `(mt * K_tiles + kt) * ROWS`. Drive `arr_a_in = sp_a_rdata`, deassert `arr_load_weight`. Drive `arr_b_in = 0`.

3. **DRAIN** (ROWS + COLS - 1 cycles): Drive `arr_a_in = 0`, `arr_b_in = 0`, keep `arr_enable` high. Capture `arr_drain_out[j]` into drain register `drain_regs[m][j]` where `m = drain_cnt - j` (`drain_cnt` counts from 0 within the DRAIN state; for PIPELINE_DEPTH=2, DRAIN starts exactly when the first valid output appears). Only capture when `0 <= m < ROWS`.

4. **WRITEBACK** (ROWS or 2*ROWS cycles): Deassert `arr_enable`. If kt == 0: write `drain_regs[row]` directly to SP_C at C tile (mt, nt) base + row offset. Takes ROWS cycles. If kt > 0: interleaved read-modify-write on the single-port C scratchpad — alternate read and write cycles (read row m, then write row m with `rdata + drain_regs[m]`, then read row m+1, etc.). Takes 2*ROWS cycles.

**Signal summary per state:**

| State | arr_enable | arr_load_weight | arr_a_in | arr_b_in |
|-------|-----------|----------------|----------|----------|
| IDLE | 0 | 0 | 0 | 0 |
| LOAD_WEIGHTS | 1 | 1 | 0 | sp_b_rdata |
| FEED | 1 | 0 | sp_a_rdata | 0 |
| DRAIN | 1 | 0 | 0 | 0 |
| WRITEBACK | 0 | 0 | 0 | 0 |
| DONE | 0 | 0 | 0 | 0 |

**Drain register file:** `logic [ACC_WIDTH-1:0] drain_regs [0:ROWS-1][0:COLS-1]`. Total size for default 4×4 INT16: 4 × 4 × 32 = 512 bits.

### `top.sv` — Top-Level Integration

Parameters:
- `ROWS`, `COLS`, `DATA_WIDTH`, `ACC_WIDTH`, `PIPELINE_DEPTH` — forwarded to array and controller
- `SP_A_DEPTH`, `SP_B_DEPTH`, `SP_C_DEPTH` — scratchpad depths (default 64 each)

Ports:

| Port | Direction | Width | Description |
|------|-----------|-------|-------------|
| `clk`, `rst_n` | input | 1 | Clock/reset |
| `start` | input | 1 | Begin computation |
| `done` | output | 1 | Computation complete |
| `M`, `K`, `N` | input | 16 | Matrix dimensions |
| `ext_sel` | input | 2 | Scratchpad select (0=A, 1=B, 2=C) |
| `ext_addr` | input | EXT_ADDR_W | Scratchpad address |
| `ext_we` | input | 1 | Write enable |
| `ext_wdata` | input | EXT_DATA_W | Write data |
| `ext_rdata` | output | EXT_DATA_W | Read data (1-cycle latency) |

Width parameters:
```systemverilog
localparam EXT_DATA_W = COLS * ACC_WIDTH;  // widest scratchpad (SP_C)
localparam EXT_ADDR_W = $clog2(SP_A_DEPTH > SP_B_DEPTH ?
                               (SP_A_DEPTH > SP_C_DEPTH ? SP_A_DEPTH : SP_C_DEPTH) :
                               (SP_B_DEPTH > SP_C_DEPTH ? SP_B_DEPTH : SP_C_DEPTH));
```

When writing to a narrower scratchpad (SP_A or SP_B), only the lower bits of `ext_wdata` are used. When reading from a narrower scratchpad, the upper bits of `ext_rdata` are zero-padded.

**Internal wiring:**
- Scratchpad address/data/we muxed between external port and controller based on controller state (IDLE = external, else = controller)
- Controller drives array signals directly
- Array drain_out fed back to controller

---

## Scratchpad Storage Layout

Matrices are stored in pre-tiled format. The host (testbench) converts from row-major to this layout before writing to scratchpads.

**SP_A** — rows of A tiles, width = ROWS * DATA_WIDTH:
- Tile (mt, kt) at base address `(mt * K_tiles + kt) * ROWS` where `K_tiles = K / ROWS`
- Entry t within tile = row t of the A sub-tile: `{A[mt*ROWS + t][kt*ROWS + ROWS-1], ..., A[mt*ROWS + t][kt*ROWS]}`
- Matches the array's a_in feeding pattern: a_in[k] = A[t][k] at feed cycle t
- Controller reads addresses base, base+1, ..., base+ROWS-1 sequentially during FEED

**SP_B** — rows of B tiles, width = COLS * DATA_WIDTH:
- Tile (kt, nt) at base address `(kt * N_tiles + nt) * ROWS` where `N_tiles = N / COLS`
- Entry t within tile = row t of the B sub-matrix: `{B[kt*ROWS + t][nt*COLS + COLS-1], ..., B[kt*ROWS + t][nt*COLS]}`
- Controller reads addresses base+ROWS-1, base+ROWS-2, ..., base (reverse) during LOAD_WEIGHTS

**SP_C** — rows of C tiles, width = COLS * ACC_WIDTH:
- Tile (mt, nt) at base address `(mt * N_tiles + nt) * ROWS` where `N_tiles = N / COLS`
- Entry t within tile = row t of the C sub-matrix: `{C[mt*ROWS + t][nt*COLS + COLS-1], ..., C[mt*ROWS + t][nt*COLS]}`
- Controller writes during WRITEBACK (direct or read-modify-write)

**C scratchpad initialization:** Scratchpad contents are undefined at reset. The tiling loop always processes kt=0 first (direct write), which initializes each C tile entry before any kt>0 pass reads it for accumulation. No pre-zeroing is needed.

---

## Testing

**Testbench: `tb/tb_top.cpp`**

Helper functions:
- `pack_a_tile()` — convert row-major A matrix into SP_A tile-column format
- `pack_b_tile()` — convert row-major B matrix into SP_B tile-row format
- `unpack_c()` — read SP_C and convert back to row-major C matrix
- `write_sp()` / `read_sp()` — drive external port to write/read scratchpad entries

Test cases:

1. **Single tile (4×4)** — M=K=N=4 on 4×4 array. One pass, no tiling, no accumulation. Verifies basic integration.

2. **K-tiling (4×8 × 8×4)** — M=4, K=8, N=4. Two K-tiles. Verifies read-modify-write accumulation in C scratchpad.

3. **Full tiling (8×8)** — M=K=N=8 on 4×4 array. 2×2×2 = 8 tile iterations. Verifies all three tile loops and address computation.

4. **Reset** — Assert `rst_n` low mid-computation, verify clean return to IDLE.

Reference values computed inline using int32 matrix multiply (same as existing array testbench), or via `ref_matmul.py`.

VCD dump to `waves/top.vcd`.

**Makefile target:** `make sim_top`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `rtl/scratchpad.sv` | Create | Generic synchronous SRAM |
| `rtl/controller.sv` | Create | Tiled matmul FSM with drain registers |
| `rtl/top.sv` | Create | Integration: scratchpads + controller + array |
| `tb/tb_top.cpp` | Create | Integration testbench with tiling tests |
| `Makefile` | Modify | Add `sim_top` target (uses `--Mdir obj_dir_top` to avoid conflicts) |
| `.gitignore` | Modify | Add `obj_dir_top/` |

---

## Implementation Tasks

### Task 1: Create `scratchpad.sv`

**Files:** Create `rtl/scratchpad.sv`

- [x] **Step 1:** Write generic synchronous SRAM module
- [x] **Step 2:** Verify compiles with Verilator

### Task 2: Create `controller.sv` (skeleton)

**Files:** Create `rtl/controller.sv`

- [x] **Step 1:** Write module with all ports, state enum, IDLE/DONE states only. All outputs driven to default values.
- [x] **Step 2:** Verify compiles with Verilator

### Task 3: Create `top.sv`

**Files:** Create `rtl/top.sv`

- [x] **Step 1:** Wire scratchpads + controller + array. External port mux (IDLE = external, else = controller).
- [x] **Step 2:** Verify compiles with Verilator

### Task 4: Makefile + `tb_top.cpp` scaffold

**Files:** Modify `Makefile`, create `tb/tb_top.cpp`

- [x] **Step 1:** Add `sim_top` target to Makefile (uses `--Mdir obj_dir_top`)
- [x] **Step 2:** Write minimal testbench: instantiate, reset, exit cleanly
- [x] **Step 3:** Verify `make sim_top` builds and runs

### Task 5: External port test

**Files:** Modify `tb/tb_top.cpp`

- [x] **Step 1:** Add `write_sp()` and `read_sp()` helpers
- [x] **Step 2:** Write test: write known values to SP_A, SP_B, SP_C, read back, verify
- [x] **Step 3:** Verify test passes

### Task 6: Controller — single tile FSM

**Files:** Modify `rtl/controller.sv`

- [x] **Step 1:** Implement LOAD_WEIGHTS state (read SP_B reverse, drive b_in)
- [x] **Step 2:** Implement FEED state (read SP_A sequential, drive a_in)
- [x] **Step 3:** Implement DRAIN state (capture drain_out into drain_regs)
- [x] **Step 4:** Implement WRITEBACK state (direct write, kt=0 only)
- [x] **Step 5:** Wire start → LOAD_WEIGHTS → FEED → DRAIN → WRITEBACK → DONE transitions
- [x] **Step 6:** Verify compiles

### Task 7: Single tile test (4×4)

**Files:** Modify `tb/tb_top.cpp`

- [x] **Step 1:** Add `pack_a_tile()`, `pack_b_tile()`, `unpack_c()` helpers
- [x] **Step 2:** Write single-tile test: M=K=N=4, load A and B, start, poll done, read C, verify
- [x] **Step 3:** Verify test passes

### Task 8: Controller — tiling loop + K-accumulation

**Files:** Modify `rtl/controller.sv`

- [x] **Step 1:** Add mt, nt, kt tile counters with proper advancement
- [x] **Step 2:** Compute SP_A, SP_B, SP_C base addresses from tile indices
- [x] **Step 3:** Implement interleaved read-modify-write WRITEBACK for kt > 0
- [x] **Step 4:** Verify compiles

### Task 9: Multi-tile tests

**Files:** Modify `tb/tb_top.cpp`

- [x] **Step 1:** Add K-tiling test: 4×8 × 8×4 (2 K-tiles)
- [x] **Step 2:** Add full tiling test: 8×8 (2×2×2 = 8 tiles)
- [x] **Step 3:** Verify both tests pass

### Task 10: Reset test + final verification

**Files:** Modify `tb/tb_top.cpp`, modify `.gitignore`

- [x] **Step 1:** Add reset test: start computation, assert rst_n low mid-way, verify IDLE recovery
- [x] **Step 2:** Add `obj_dir_top/` to `.gitignore` and `clean` target
- [x] **Step 3:** Run `make sim_top` — all tests pass

---

Milestone 4 is complete when all four test cases pass: single-tile 4×4, K-tiled 4×8×8×4, fully-tiled 8×8, and reset recovery.
