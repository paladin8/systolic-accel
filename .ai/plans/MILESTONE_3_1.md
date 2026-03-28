# Milestone 3.1: Place & Route for Timing Analysis

**Goal:** Run nextpnr-ice40 place-and-route on all synthesis configurations to get actual critical path timing, then compute real throughput (MACs/second) to determine the optimal pipeline depth.

**Architecture:** The existing M3 flow synthesizes with Yosys (`synth_ice40`) and reports LUT/FF counts. This milestone adds a P&R step: Yosys outputs a JSON netlist, nextpnr-ice40 places and routes it on a target device, and reports the critical path delay. The analysis script then combines cycle counts with timing to compute actual throughput.

**Tech Stack:** nextpnr-ice40 (apt install), Yosys 0.33, Python, existing `run_sweep.py` and `analyze.py`

**Spec reference:** `.ai/plans/MILESTONE_3.md` (builds on M3 synthesis infrastructure)

---

## Design Decisions

1. **Target device: iCE40 HX8K** (7680 LUTs) — Fits 4×4 INT8 (3107 LUTs), 8×8 INT8 (fits), and smaller configs. Does NOT fit 4×4 INT16 (~12-17k LUTs) or 16×16 INT8 (50k LUTs). For configs that don't fit, P&R is skipped and Fmax reported as N/A. The **depth sweep is run at 4×4 INT8** (not INT16) so all three depths fit on the device — the relative Fmax trends between depths are what matter, not the absolute data width.

2. **Unconstrained P&R** — No pin constraints (PCF file) or clock constraints. nextpnr reports the achievable Fmax for the design's critical path. This is sufficient for relative comparisons across pipeline depths.

3. **Extend existing scripts** — Add P&R to `run_sweep.py` (Yosys writes JSON, nextpnr runs on it) and timing analysis to `analyze.py`. No new scripts needed.

4. **Depth sweep at INT8** — M3 ran the depth sweep at 4×4 INT16, but those designs (12-17k LUTs) don't fit on HX8K. The P&R depth sweep uses 4×4 INT8 (ACC_WIDTH=16) instead. `run_sweep.py` adds a new experiment `4_depth_pnr` with INT8 configs at depths 1, 2, 3.

5. **Throughput formula** — `throughput = N² × Fmax / compute_cycles` where `compute_cycles = (2N - 1) + (PIPELINE_DEPTH - 1)`. This gives MACs/second, the real performance metric.

---

## Flow

```
Yosys                          nextpnr-ice40
─────                          ─────────────
read RTL                       read JSON netlist
synth_ice40                    place & route on HX8K
write_json → netlist.json  →   report timing (Fmax)
stat (LUTs/FFs)                write timing report
```

For each configuration:
1. Yosys synthesizes and writes `synth/logs/<config>.json`
2. nextpnr-ice40 reads the JSON, places and routes, reports Fmax
3. Results (LUTs, FFs, Fmax) saved to `synth/results.json`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `synth/run_sweep.py` | Modify | Add Yosys JSON output + nextpnr P&R step |
| `synth/analyze.py` | Modify | Add Fmax column to tables, compute real throughput, new charts |
| `Makefile` | No change | `make synth` already runs sweep + analyze |

---

## Implementation Tasks

### Task 1: Install nextpnr-ice40

- [ ] **Step 1: Install via apt**

```bash
sudo apt install -y nextpnr-ice40
```

- [ ] **Step 2: Verify installation**

```bash
nextpnr-ice40 --version
```

---

### Task 2: Update run_sweep.py with P&R step

**Files:**
- Modify: `synth/run_sweep.py`

- [ ] **Step 1: Add JSON netlist output to Yosys command**

After `synth_ice40` and `stat`, add `write_json <path>` to the Yosys command. Output to `synth/logs/<config>.json`.

- [ ] **Step 2: Add nextpnr-ice40 invocation**

After Yosys completes, run:
```bash
nextpnr-ice40 --hx8k --package ct256 --json <config>.json --freq 1 --report <config>_timing.json
```

- `--hx8k --package ct256`: target device (largest common iCE40)
- `--freq 1`: minimal frequency constraint (just want Fmax report)
- `--report`: JSON timing report
- No `--asc` output needed (we don't need a bitstream)
- No `--pcf` (unconstrained)

If nextpnr fails (design too large for device), log the error and set Fmax to null.

- [ ] **Step 3: Parse Fmax from nextpnr output**

nextpnr prints `Max frequency for clock 'clk': XX.XX MHz` to stdout. Parse this and add to results. Also check the JSON report if stdout parsing is unreliable.

- [ ] **Step 4: Test with one config**

Run: `uv run python synth/run_sweep.py 3_depth`
Expected: Each config gets synthesis + P&R. Fmax reported for configs that fit on HX8K.

---

### Task 3: Update analyze.py with timing and throughput

**Files:**
- Modify: `synth/analyze.py`

- [ ] **Step 1: Add Fmax to all experiment tables**

Add "Fmax (MHz)" column to each table. Show "N/A" for configs that failed P&R.

- [ ] **Step 2: Update depth analysis with real throughput**

For the pipeline depth experiment:
- Compute `throughput = N² × Fmax / compute_cycles` (in M MACs/sec)
- Replace the cycle-only throughput chart with actual throughput
- Add a critical path chart showing Fmax vs depth
- Keep the area chart as-is

New charts:
- `docs/fmax_vs_depth.png` — Fmax (MHz) for each pipeline depth
- `docs/throughput_vs_depth.png` — updated with real MACs/second (replaces old cycle-only version)

- [ ] **Step 3: Test analysis**

Run: `uv run python synth/analyze.py`
Expected: Tables include Fmax. Throughput chart shows MACs/second.

---

### Task 4: Run full sweep and verify

- [ ] **Step 1: Run full sweep with P&R**

Run: `make synth`
Expected: All configs synthesized and placed/routed (where they fit). Results in `synth/results.json`.

- [ ] **Step 2: Verify results answer the key question**

The pipeline depth throughput chart should show which depth gives the best MACs/second:
- Depth 1: fewer cycles but lower Fmax (long combinational path)
- Depth 2: more cycles but higher Fmax (registered multiply)
- Depth 3: most cycles, highest Fmax (most pipelining) — if the Fmax gain is enough

---

Milestone 3.1 is complete when the throughput chart shows MACs/second for each pipeline depth, answering the question: does depth 2's frequency advantage outweigh its extra cycles compared to depth 1?
