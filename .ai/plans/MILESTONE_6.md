# Milestone 6: Double-Buffered Scratchpads

## Goal

Add double-buffered SP_A, SP_B, and SP_C scratchpads so the host can load the next matrix's input data while the controller is still computing the current matrix, and read previous results while the next computation is in progress. This eliminates the load-compute-readback serialization at the system level.

## Output

- Modified `rtl/top.sv` — duplicate SP_A/SP_B/SP_C instances, bank muxing, new ports
- Modified `tb/tb_top.cpp` — new tests for bank select, double-buffering, and active bank protection
- Synthesis comparison — area delta vs. single-buffered baseline
- Updated `docs/ARCHITECTURE.md` — new subsection on double-buffering

No changes to `rtl/scratchpad.sv`, `rtl/systolic_array.sv`, or `rtl/mac_unit.sv`. Minor Yosys compatibility fix in `rtl/controller.sv` (removed `automatic` keyword unsupported by Yosys 0.33 — functionally equivalent).

## Design

### Memory Architecture

Three additional scratchpad instances (sp_a1, sp_b1, sp_c1), bringing the total to six:

| Instance | Width | Depth | Purpose |
|----------|-------|-------|---------|
| sp_a0 | ROWS × DATA_WIDTH | SP_A_DEPTH | Activation bank 0 |
| sp_a1 | ROWS × DATA_WIDTH | SP_A_DEPTH | Activation bank 1 |
| sp_b0 | COLS × DATA_WIDTH | SP_B_DEPTH | Weight bank 0 |
| sp_b1 | COLS × DATA_WIDTH | SP_B_DEPTH | Weight bank 1 |
| sp_c0 | COLS × ACC_WIDTH | SP_C_DEPTH | Result bank 0 |
| sp_c1 | COLS × ACC_WIDTH | SP_C_DEPTH | Result bank 1 |

Each scratchpad instance remains single-port and unchanged. All bank muxing is in `top.sv`.

### New Ports on `top.sv`

```systemverilog
input  logic  bank_sel,   // which A/B bank the controller reads from
input  logic  ext_bank,   // which A/B bank the external host accesses
```

### Bank Select Latching

`bank_sel` is sampled into a register (`active_bank_reg`) when `start` is asserted, and held for the duration of computation. To avoid a one-cycle hazard (the controller pre-reads SP_B in S_IDLE on the same cycle `start` is high), the effective `active_bank` signal is combinational:

```systemverilog
assign active_bank = (start && !ctrl_running_reg) ? bank_sel : active_bank_reg;
```

This ensures the mux routes to the correct bank in the same cycle `start` is asserted, before `active_bank_reg` updates at the next posedge. `active_bank_reg` resets to 0.

The controller reads from `sp_a{active_bank}` and `sp_b{active_bank}`. Mid-computation changes to `bank_sel` have no effect.

### Access Rules

Each bank is its own single-port scratchpad instance. Two different banks can be accessed simultaneously (they are independent memories). The same bank cannot be accessed by both controller and host simultaneously.

**During computation (controller running):**

- Controller exclusively owns `sp_a{active_bank}`, `sp_b{active_bank}`, and `sp_c{active_bank}` — drives their addr/we/wdata ports
- Host exclusively owns `sp_a{!active_bank}`, `sp_b{!active_bank}`, and `sp_c{!active_bank}` — can read and write freely via `ext_bank`
- If `ext_bank == active_bank` during computation: host writes are silently ignored, host reads return data at the controller's address (not the host's requested address) — effectively undefined from the host's perspective

**During idle:**

- Host can access any bank via `ext_bank`. The selected bank's port is driven by the host's address/we/wdata
- Non-selected banks have `we` tied to 0 and `addr` driven to 0 (no floating signals)

### Mux Logic

Each of the six bank instances has one port. The mux logic assigns port drivers for each instance independently:

**Controller's banks** (`sp_a{active_bank}`, `sp_b{active_bank}`, `sp_c{active_bank}`):
- During computation: controller drives addr/we/wdata. For A/B: we=0 (read-only). For C: controller drives we and wdata (RMW writeback). Controller's rdata comes from these instances.
- During idle: host drives addr/we/wdata (if `ext_bank == active_bank`). Otherwise addr=0, we=0.

**Host's banks** (`sp_a{!active_bank}`, `sp_b{!active_bank}`, `sp_c{!active_bank}`):
- During computation: host drives addr/we/wdata via `ext_bank`. Host's `ext_rdata` comes from these instances.
- During idle: host drives addr/we/wdata (if `ext_bank != active_bank`). Otherwise addr=0, we=0.

**Concretely**, each bank instance's port is driven by exactly one of:
1. Controller signals (during computation, if this is the active bank)
2. Host signals (during computation, if this is the non-active bank; or during idle, if `ext_bank` selects it)
3. Default (addr=0, we=0, wdata=0) — when nobody needs this bank

**`ext_rdata` mux**: For `ext_sel=0`, `ext_rdata` comes from `sp_a{ext_bank}.rdata`. For `ext_sel=1`, from `sp_b{ext_bank}.rdata`. For `ext_sel=2`, from `sp_c{ext_bank}.rdata`. Width slicing of `ext_wdata` is unchanged per scratchpad type.

**SP_C clearing is unnecessary**: The controller always does a direct write for kt=0 (overwriting stale data) before doing RMW for kt>0. Each SP_C address is written before it is read within a single computation.

### Controller Changes

None. The controller still sees `sp_a_rdata`, `sp_b_rdata`, `sp_c_rdata` — it is unaware of banks. All muxing is in `top.sv`.

## Testbench

### Existing Tests

All existing tests (`test_single_tile`, `test_k_tiling`, `test_full_tiling`) continue to work with `bank_sel=0`, `ext_bank=0`. Only change: drive the new ports to 0. Existing helper functions (`write_sp64`, `pack_a_tiles`, `pack_b_tiles`) are unchanged; the caller sets `dut->ext_bank` before invoking them to select the target bank.

### New Test: `test_bank_select`

Load identical A/B matrices into both banks. Run computation on bank 0, verify results. Run on bank 1, verify same results. Confirms both banks are independently functional.

### New Test: `test_double_buffer`

Demonstrates the full ping-pong workflow with three distinct matrix pairs (A0/B0, A1/B1, A2/B2) using values that can't be confused with each other.

**Sequence:**

1. Load A0/B0 into bank 0
2. Load A1/B1 into bank 1
3. Start computation with `bank_sel=0`
4. While running: read back bank 1 data, verify A1/B1 is intact (host can read non-active bank during compute)
5. While running: overwrite bank 1 with A2/B2 (host can write non-active bank during compute)
6. Wait for done
7. Read SP_C, verify matches `ref_matmul(A0, B0)`
8. Start computation with `bank_sel=1` — immediate, no loading gap
9. While running: reload A0/B0 into bank 0 (prep for re-use)
10. Wait for done
11. Read SP_C, verify matches `ref_matmul(A2, B2)` — NOT A1/B1, proving the mid-compute overwrite (step 5) took effect
12. Start computation with `bank_sel=0`
13. Wait for done
14. Read SP_C, verify matches `ref_matmul(A0, B0)` — full round-trip, bank 0 reloaded in step 9

**Cycle count verification:** Second and third computations take the same cycle count as the first — no overhead from bank switching.

**Tiled variant:** Run the same ping-pong sequence with an 8x8 matrix on the 4x4 array (full tiling with K-dimension accumulation) to verify bank select interacts correctly with the multi-tile controller loop and RMW accumulation in SP_C.

### New Test: `test_active_bank_protection`

Write to `ext_bank=0` while controller computes on `bank_sel=0`. After computation finishes, read back the bank and verify the original data is intact (write was silently ignored).

## Synthesis

Re-run the 4x4 INT8 PD=2 synthesis (same configuration as M3's baseline) with the double-buffered design. Report:

- LUTs, FFs, total cells (compare to single-buffered baseline from `synth/results.json`)
- Fmax from P&R (compare to baseline from `synth/results.json`)
- Expected: roughly 2x SP_A + SP_B register area, small mux overhead, Fmax unchanged

Single data point — no full sweep needed.

## Architecture Doc Update

Add a subsection to `docs/ARCHITECTURE.md` Section 4 (Memory Bandwidth and Scale) describing:

- What double-buffering overlaps (host load hidden behind compute)
- Area cost from synthesis
- System-level throughput model change: in steady state, throughput is `max(compute_time, load_time)` instead of `compute_time + load_time`

## Constraints

- No changes to scratchpad.sv, systolic_array.sv, or mac_unit.sv
- All existing tests must continue to pass unchanged (with new ports driven to 0)
- `bank_sel` must be latched — changing it mid-computation has no effect
- SP_C RMW accumulation across K-tiles is safe: kt=0 always does direct write before kt>0 does RMW, so stale data in the bank is always overwritten first
