# Milestone 7: Formal Verification

## Goal

Add formal verification to all five RTL modules using SymbiYosys and inline SVA assertions. Build up from simple leaf modules (scratchpad, MAC) through the systolic array to the complex controller FSM and top-level bank arbitration. Prove safety, liveness, and functional correctness properties exhaustively for all reachable states.

## Output

- Inline `ifdef FORMAL` assertion blocks in all 5 RTL files
- `formal/*.sby` — 5 SymbiYosys task files (one per module; mac_unit has 3 tasks for pipeline depth variants)
- `Makefile` additions — `make formal_sp`, `make formal_mac`, `make formal_array`, `make formal_ctrl`, `make formal_top`, `make formal` (all)
- SymbiYosys + solver installation (yices2 primary, z3 fallback)

No changes to testbenches, synthesis scripts, or architecture doc. No changes to RTL logic (unless formal reveals a bug).

## Tooling

### Installation

```bash
# SymbiYosys (formal verification frontend)
git clone https://github.com/YosysHQ/sby
cd sby && sudo make install

# Yices2 (primary SMT solver — fast for bitvector BMC)
sudo apt-get install yices2

# Z3 (fallback solver — slower but more robust)
sudo apt-get install z3
```

Requires Yosys (already installed at 0.33). SymbiYosys invokes Yosys internally — no version-specific issues expected for basic BMC/induction flows.

### SymbiYosys

SymbiYosys (`sby`) is a formal verification frontend for Yosys. It reads `.sby` task files that specify:
- Which RTL files to read
- Which solver to use (yices2 for BMC, z3 as fallback)
- Proof mode: `bmc` (bounded model checking — explores all states up to depth N) or `prove` (induction — proves properties hold for all reachable states)
- BMC depth (number of cycles to unroll)

### Assertion Language

Properties are written as inline SystemVerilog Assertions (SVA) inside `` `ifdef FORMAL `` guards:
- `assert property (...)` — must always hold; sby reports FAIL with counterexample if violated
- `assume property (...)` — constrains the solver's input space (e.g., valid reset sequence)
- `cover property (...)` — asks the solver to find a trace reaching this condition (reachability check)
- `restrict property (...)` — like assume but only in formal mode

Properties use `$past(signal)`, `$rose(signal)`, `$fell(signal)`, `$stable(signal)` temporal operators, plus standard SVA `|->` (implication) and `##N` (cycle delay).

### Verilator Integration

Verilator natively supports `assert` statements — immediate assertions outside `ifdef FORMAL` fire during simulation. Concurrent SVA properties inside `ifdef FORMAL` are only seen by SymbiYosys. This gives dual-use: assertions validate during simulation AND are proven exhaustively by formal.

### Common Formal Infrastructure

Every module with `rst_n` uses this standard reset assumption pattern:

```systemverilog
`ifdef FORMAL
    // -- Past-valid tracker (needed for $past to be meaningful) --
    logic f_past_valid;
    initial f_past_valid = 1'b0;
    always_ff @(posedge clk)
        f_past_valid <= 1'b1;

    // -- Reset assumption: start in reset, once released stay released --
    initial assume (!rst_n);
    always_ff @(posedge clk)
        if (f_past_valid && $past(rst_n))
            assume (rst_n);
`endif
```

All assertions using `$past()` must be gated on `f_past_valid` (and typically `rst_n` too) to avoid firing on the first cycle when `$past` returns X.

**Note:** `scratchpad.sv` has no `rst_n` port. Its formal proofs start from an unconstrained memory state, which is actually more thorough — the solver explores all possible initial memory contents.

## Design

### Module Order

Bottom-up dependency order:

1. **scratchpad.sv** — simplest module, learn the sby workflow
2. **mac_unit.sv** — pipeline verification, parameterized depth
3. **systolic_array.sv** — interconnect and skewing (trusts MAC correctness)
4. **controller.sv** — complex FSM, tiling loops, address sequencing
5. **top.sv** — bank arbitration, mutual exclusion

### 1. Scratchpad Properties

**Shadow register technique:** Formal tools can't directly observe internal memory arrays. We declare a formal-only address `f_addr` (chosen by the solver via `(* anyconst *)`) and a shadow register `f_shadow` that tracks `mem[f_addr]`. Properties are written against `f_shadow`. The solver explores all possible values of `f_addr`, giving universal coverage.

Since the scratchpad has no reset, `mem` starts in an unconstrained state. We use a `f_written` tracking bit to gate assertions — properties only fire after at least one write to `f_addr`, so we know `f_shadow` matches `mem[f_addr]`.

```systemverilog
`ifdef FORMAL
    (* anyconst *) logic [$clog2(DEPTH)-1:0] f_addr;
    logic [WIDTH-1:0] f_shadow;
    logic f_written;
    logic f_past_valid;

    initial begin
        f_past_valid = 1'b0;
        f_written = 1'b0;
    end

    always_ff @(posedge clk) begin
        f_past_valid <= 1'b1;
        if (we && addr == f_addr) begin
            f_shadow <= wdata;
            f_written <= 1'b1;
        end
    end
`endif
```

**Properties:**

| # | Name | Type | Description |
|---|------|------|-------------|
| SP1 | Read-after-write | assert | `f_past_valid && f_written && addr == f_addr && !we` → `rdata == f_shadow` (read returns last written value). Gated on `f_written` to avoid comparing against uninitialized shadow |
| SP2 | Read stability | assert | `f_past_valid && $stable(addr) && !we && !$past(we)` → `$stable(rdata)` |
| SP3 | Write isolation | assert | `f_past_valid && f_written && $past(we) && $past(addr) != f_addr` → `f_shadow == $past(f_shadow)` (writing to another address doesn't change tracked address) |
| SP4 | Read-first | assert | One cycle after a simultaneous read+write to `f_addr` (i.e., `$past(we && addr == f_addr && f_written)`), `rdata` equals the pre-write `$past(f_shadow)`, not the newly written value. This confirms read-first semantics through the registered output |
| SP5 | Reachability | cover | Solver reaches: write to f_addr, then read from f_addr returns the written value |

**BMC depth:** 5 cycles.

### 2. MAC Unit Properties

Three pipeline depth configurations verified via separate sby tasks.

**Past-value tracking:** Formal-only shift registers track past inputs to express "output equals input from N cycles ago":

```systemverilog
`ifdef FORMAL
    // f_past_valid and reset assumption from common infrastructure

    logic [DATA_WIDTH-1:0] f_past_a;
    logic [$clog2(8)-1:0] f_enable_count;  // track pipeline fullness

    initial f_enable_count = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            f_past_a <= '0;
            f_enable_count <= '0;
        end else if (enable) begin
            f_past_a <= a;
            if (f_enable_count < 7)
                f_enable_count <= f_enable_count + 1'b1;
        end
    end
`endif
```

**Properties:**

All compute properties (MAC7-9) require `f_past_valid && rst_n && $past(rst_n)` guards. The multiply uses signed extension: `ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg))` — properties must match this signed semantics.

| # | Name | Type | Description |
|---|------|------|-------------|
| MAC1 | Weight latch | assert | `f_past_valid && rst_n && $past(rst_n && enable && load_weight)` → `weight_reg == $past(b)` |
| MAC2 | Weight stability | assert | `f_past_valid && rst_n && $past(rst_n && !(enable && load_weight))` → `weight_reg == $past(weight_reg)` |
| MAC3 | Reset clears | assert | `f_past_valid && rst_n && !$past(rst_n)` → all registers are 0 |
| MAC4 | Activation fwd | assert | `f_past_valid && rst_n && $past(rst_n && enable)` → `a_out == $past(a)` |
| MAC5 | Weight fwd | assert | `f_past_valid && rst_n && $past(rst_n && enable)` → `b_out == $past(b)` |
| MAC6 | Stall hold | assert | `f_past_valid && rst_n && $past(rst_n && !enable)` → `a_out == $past(a_out) && b_out == $past(b_out)` |
| MAC7 | Compute D1 | assert | Depth 1 only. `f_past_valid && rst_n && $past(rst_n && enable)` → `psum_out == $past(psum_in + signed_ext(a) * signed_ext(weight_reg))` |
| MAC8 | Compute D2 | assert | Depth 2 only. After 2+ enabled cycles: `psum_out == $past(psum_in) + $past(mult_reg)` where mult_reg tracks the registered product |
| MAC9 | Compute D3 | assert | Depth 3 only. After 3+ enabled cycles: output reflects 3-cycle pipeline |
| MAC10 | Cover compute | cover | Solver reaches a state with nonzero a, nonzero weight, nonzero psum_in, and correct psum_out |

**sby config:** One file with `[tasks]` section defining `depth1`, `depth2`, `depth3`. Each task sets `PIPELINE_DEPTH` via `chparam`. BMC depth: 8 cycles.

### 3. Systolic Array Properties

The MAC is already proven correct — here we verify the interconnect and skewing. Compositional verification: trust the leaves, prove the wiring.

**Properties:**

| # | Name | Type | Description |
|---|------|------|-------------|
| ARR1 | Row 0 direct | assert | `a_wire[0][0] == a_in[0 +: DATA_WIDTH]` — no skew delay on row 0 |
| ARR2 | Row k delay | assert | Activation entering row k arrives k cycles after presented at a_in[k]. Proved via formal-only delayed copies of each row input |
| ARR3 | Skew stall | assert | When !enable, all skew shift registers hold (values stable) |
| ARR4 | Psum zero top | assert | `psum_wire[0][j] == 0` for all j (top row gets zero partial sum) |
| ARR5 | Weight top edge | assert | `b_wire[0][j] == b_in[j*DATA_WIDTH +: DATA_WIDTH]` for all j |
| ARR6 | Drain output | assert | `drain_out[j*ACC_WIDTH +: ACC_WIDTH] == psum_wire[ROWS][j]` for all j |
| ARR7 | Cover skew output | cover | Solver reaches a state where `a_wire[ROWS-1][0]` is nonzero (last row's skew register has propagated a value) |

Properties ARR1, ARR4-ARR6 are combinational (depth 0). ARR2-ARR3 need BMC depth >= ROWS + 2.

**BMC depth:** ROWS + 2 = 6 cycles.

### 4. Controller Properties

The most complex module. Properties grouped by category.

**Assume constraints:** Valid dimensions — `dim_m`, `dim_k`, `dim_n` are nonzero multiples of ROWS/COLS, and small enough that tile counts fit in 16 bits. `start` is a single-cycle pulse (high for exactly one cycle, then low until done).

#### FSM Structural Safety

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL1 | Legal states | assert | `state` is always in {S_IDLE..S_DONE} |
| CTRL2 | Valid transitions | assert | State transitions follow the allowed graph: S_IDLE→S_LOAD_WEIGHTS, S_LOAD_WEIGHTS→S_FEED, S_FEED→S_DRAIN, S_DRAIN→S_WRITEBACK, S_WRITEBACK→{S_DONE, S_LOAD_WEIGHTS, S_FEED}, S_DONE→S_DONE |
| CTRL3 | S_DONE absorbing | assert | Once state reaches S_DONE, it stays in S_DONE until reset (terminal state) |
| CTRL4 | Phase reset | assert | When state changes (`state != $past(state)`), phase_cnt is 0 |

#### Liveness

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL5 | Phase bounded | assert | phase_cnt never exceeds the max for current state: ROWS-1 for LOAD_WEIGHTS and FEED, DRAIN_CYCLES-1 for DRAIN, wb_last for WRITEBACK |
| CTRL6 | Termination | cover | For constrained valid dimensions (dim_m=dim_k=dim_n=4, single tile), reach S_DONE. Multi-tile termination (dim_m=8) covered by CTRL22. Full induction-based termination proof is optional — requires auxiliary invariants showing tile indices form a monotonically decreasing measure |

#### Scratchpad Address Sequencing

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL7a | IDLE pre-read | assert | When `start` in S_IDLE, `sp_b_addr == ROWS-1` (first tile, b_base=0) |
| CTRL7b | WB→LW pre-read | assert | On last WRITEBACK cycle when `next_needs_load && more_tiles`, `sp_b_addr == next_b_base + ROWS - 1` |
| CTRL8 | LOAD_WEIGHTS addr | assert | In S_LOAD_WEIGHTS at phase_cnt < ROWS-1, `sp_b_addr == b_base + (ROWS-2) - phase_cnt` (reverse order, one step ahead) |
| CTRL9a | LW→FEED pre-read | assert | On last LOAD_WEIGHTS cycle (phase_cnt == ROWS-1), `sp_a_addr == a_base` |
| CTRL9b | WB→FEED pre-read | assert | On last WRITEBACK cycle when `!next_needs_load && more_tiles`, `sp_a_addr == next_a_base` |
| CTRL10 | FEED addr | assert | In S_FEED at phase_cnt < ROWS-1, `sp_a_addr == a_base + phase_cnt + 1` (forward order, one step ahead) |
| CTRL11 | WB direct addr | assert | When kt==0 in WRITEBACK, `sp_c_addr == c_base + phase_cnt`, `sp_c_we==1` |
| CTRL12a | WB RMW write | assert | When kt!=0 in WRITEBACK, even phase_cnt: `sp_c_we==1`, `sp_c_addr == c_base + (phase_cnt >> 1)`. Note: phase_cnt=0 write uses rdata from the DRAIN-phase pre-read (CTRL12c), not a WRITEBACK read |
| CTRL12b | WB RMW read | assert | When kt!=0 in WRITEBACK, odd phase_cnt: `sp_c_we==0`, `sp_c_addr == c_base + (phase_cnt >> 1) + 1` |
| CTRL12c | DRAIN→WB pre-read | assert | On last DRAIN cycle when `wb_is_rmw`, `sp_c_addr == c_base` (pre-read row 0 for first RMW write) |
| CTRL13 | WB direct data | assert | Direct write: `sp_c_wdata[j*ACC_WIDTH +: ACC_WIDTH] == drain_regs[phase_cnt][j]` |
| CTRL14 | WB RMW data | assert | RMW write: `sp_c_wdata[j*ACC_WIDTH +: ACC_WIDTH] == sp_c_rdata[j*ACC_WIDTH +: ACC_WIDTH] + drain_regs[wb_row][j]` |

#### Drain Capture

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL15 | Drain timing | assert | `drain_regs[m][j]` written iff `state==S_DRAIN && phase_cnt == m + j + 1` (within bounds: phase_cnt >= j+1 && phase_cnt < j+1+ROWS) |

#### Tile Loop

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL16 | Tile advance | assert | At WRITEBACK end, (mt, kt, nt) advances correctly: mt innermost, kt middle, nt outermost |
| CTRL17 | next_needs_load | assert | `next_needs_load` is true iff `!(mt + 1 < mt_max)` (kt or nt will change) |

#### Array Control

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL18 | load_weight timing | assert | `arr_load_weight` high only in S_LOAD_WEIGHTS |
| CTRL19 | enable timing | assert | `arr_enable` high in S_LOAD_WEIGHTS, S_FEED, S_DRAIN; low otherwise |
| CTRL20 | feed data | assert | In S_FEED: `arr_a_in == sp_a_rdata`. In S_LOAD_WEIGHTS: `arr_b_in == sp_b_rdata` |

#### Cover Traces

| # | Name | Type | Description |
|---|------|------|-------------|
| CTRL21 | Single tile done | cover | Reach S_DONE with mt_max=nt_max=kt_max=1 |
| CTRL22 | Multi-tile done | cover | Reach S_DONE with at least 2 tiles (e.g., dim_m=8, dim_k=dim_n=4 → 2x1x1) |
| CTRL23 | RMW writeback | cover | Reach WRITEBACK with kt!=0 (RMW path exercised) |
| CTRL24 | Weight reuse | cover | Reach a WRITEBACK→FEED transition (mt advances, weights reused) |

**BMC depth:** 30 for single-tile properties, 80-100 for multi-tile. Induction for CTRL2, CTRL3, CTRL5.

### 5. Top-Level Properties

**State-space management:** The top module instantiates 6 scratchpad instances (each 64-deep), the controller, and the systolic array (16 MACs). This is a large state space for formal. To keep proofs tractable:

- Use `chparam` in the .sby file to reduce parameters: `ROWS=2, COLS=2, SP_A_DEPTH=4, SP_B_DEPTH=4, SP_C_DEPTH=4`. This reduces scratchpad memory by ~32x and MAC count by 4x while preserving all mux/arbitration logic.
- Most top-level properties are about mux routing and bank arbitration — these are combinational or shallow-depth, so even reduced parameters provide full coverage.
- A `formal_top_quick` target with ROWS=2/DEPTH=4 runs first; `formal_top` with default params can be attempted but may be slow.

**Bank Latching:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP1 | Latch on start | assert | `f_past_valid && rst_n && $past(start && !ctrl_running_reg)` → `active_bank_reg == $past(bank_sel)` |
| TOP2 | Hold during compute | assert | `f_past_valid && rst_n && ctrl_running_reg && !done && $past(rst_n)` → `$stable(active_bank_reg)` |
| TOP3 | Combinational bypass | assert | `start && !ctrl_running_reg` → `active_bank == bank_sel` |

**Mutual Exclusion:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP4 | No dual-write | assert | For each of the 6 bank instances: at most one of (controller we, host we) is asserted per cycle. (Controller never writes A/B, but property covers the general invariant.) |
| TOP5 | No addr contention | assert | During computation, active bank addr driven by controller; non-active by host or default |
| TOP6 | Single driver | assert | Each bank's (addr, we, wdata) comes from exactly one source per cycle |

**Controller Isolation:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP7 | Ctrl reads active | assert | `ctrl_a_rdata == (active_bank ? sp_a1_rdata : sp_a0_rdata)`. Same for B and C |
| TOP8 | Host reads ext_bank | assert | ext_rdata reflects data from sp_{ext_bank}, muxed by ext_sel |

**Host Protection:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP9 | Active bank write block | assert | `ctrl_running && ext_bank==active_bank` → active bank's we is not driven by host ext_we |
| TOP10 | Inactive bank access | assert | `ctrl_running && ext_bank!=active_bank` → host signals (ext_addr, ext_we, ext_wdata) correctly drive the inactive bank |

**Default Tie-off:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP11 | Unused quiescent | assert | Banks not selected by controller or host have we=0, addr=0 |

**ctrl_running:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP12 | Running tracks FSM | assert | ctrl_running_reg high from cycle after start until done |
| TOP13 | Running includes start | assert | ctrl_running (combinational) high on same cycle as start |

**Cover:**

| # | Name | Type | Description |
|---|------|------|-------------|
| TOP14 | Ping-pong | cover | Reach a state where computation completes on bank 0, then starts on bank 1 |

**BMC depth:** 5-10 cycles for most. Mutual exclusion properties are shallow (combinational/depth 1-2). Bank latching stability uses induction.

**Assume constraints:** Same valid-dimension constraints as controller. Additionally, `ext_sel` in {0,1,2}.

## .sby File Format

Example for scratchpad:

```
[tasks]
bmc

[options]
bmc: mode bmc
bmc: depth 5

[engines]
smtbmc yices

[script]
read -formal -sv rtl/scratchpad.sv
prep -top scratchpad

[files]
rtl/scratchpad.sv
```

Example for mac_unit (multi-task):

```
[tasks]
depth1
depth2
depth3

[options]
mode bmc
depth 8

[engines]
smtbmc yices

[script]
read -formal -sv rtl/mac_unit.sv
depth1: chparam -set PIPELINE_DEPTH 1 mac_unit
depth2: chparam -set PIPELINE_DEPTH 2 mac_unit
depth3: chparam -set PIPELINE_DEPTH 3 mac_unit
prep -top mac_unit

[files]
rtl/mac_unit.sv
```

Example for systolic_array (multi-file):

```
[tasks]
bmc

[options]
bmc: mode bmc
bmc: depth 6

[engines]
smtbmc yices

[script]
read -formal -sv rtl/mac_unit.sv
read -formal -sv rtl/systolic_array.sv
prep -top systolic_array

[files]
rtl/mac_unit.sv
rtl/systolic_array.sv
```

Example for top (all RTL files, reduced parameters):

```
[tasks]
bmc

[options]
bmc: mode bmc
bmc: depth 10

[engines]
smtbmc yices

[script]
read -formal -sv rtl/scratchpad.sv
read -formal -sv rtl/mac_unit.sv
read -formal -sv rtl/systolic_array.sv
read -formal -sv rtl/controller.sv
read -formal -sv rtl/top.sv
chparam -set ROWS 2 top
chparam -set COLS 2 top
chparam -set SP_A_DEPTH 4 top
chparam -set SP_B_DEPTH 4 top
chparam -set SP_C_DEPTH 4 top
chparam -set EXT_ADDR_W 2 top
prep -top top

[files]
rtl/scratchpad.sv
rtl/mac_unit.sv
rtl/systolic_array.sv
rtl/controller.sv
rtl/top.sv
```

## Makefile Targets

```makefile
.PHONY: formal_sp formal_mac formal_array formal_ctrl formal_top formal

formal_sp:
	sby -f formal/scratchpad.sby

formal_mac:
	sby -f formal/mac_unit.sby

formal_array:
	sby -f formal/systolic_array.sby

formal_ctrl:
	sby -f formal/controller.sby

formal_top:
	sby -f formal/top.sby

formal: formal_sp formal_mac formal_array formal_ctrl formal_top
```

## Constraints

- No changes to RTL logic — only additive `ifdef FORMAL` blocks
- No changes to testbenches or synthesis scripts
- All existing simulation tests must continue to pass
- Properties must be self-contained per module (no cross-module formal assertions)
- sby task files must all pass with `sby -f` (no manual intervention)
- Cover traces should be generated for at least one interesting scenario per module

## Implementation Plan

### Task 1: Install SymbiYosys and verify toolchain

- [ ] Install SymbiYosys: `git clone https://github.com/YosysHQ/sby && cd sby && sudo make install`
- [ ] Install yices2: `sudo apt-get install -y yices2`
- [ ] Install z3: `sudo apt-get install -y z3`
- [ ] Verify: `sby --help`, `yices-smt2 --version`, `z3 --version`
- [ ] Create `formal/` directory
- [ ] Add formal targets to Makefile (append after existing targets):

```makefile
.PHONY: formal_sp formal_mac formal_array formal_ctrl formal_top formal

formal_sp:
	sby -f formal/scratchpad.sby

formal_mac:
	sby -f formal/mac_unit.sby

formal_array:
	sby -f formal/systolic_array.sby

formal_ctrl:
	sby -f formal/controller.sby

formal_top:
	sby -f formal/top.sby

formal: formal_sp formal_mac formal_array formal_ctrl formal_top
```

- [ ] Add `formal/scratchpad_bmc/` (and similar sby output dirs) to `.gitignore`
- [ ] Verify `make sim_top` still passes (baseline sanity check)

### Task 2: Scratchpad formal properties (SP1-SP5)

**Files:** `rtl/scratchpad.sv`, `formal/scratchpad.sby`

- [ ] Create `formal/scratchpad.sby`:

```
[tasks]
bmc

[options]
bmc: mode bmc
bmc: depth 5

[engines]
smtbmc yices

[script]
read -formal -sv rtl/scratchpad.sv
prep -top scratchpad

[files]
rtl/scratchpad.sv
```

- [ ] Add `ifdef FORMAL` block to `rtl/scratchpad.sv` (before `endmodule`):
  - Declare `(* anyconst *) f_addr`, `f_shadow`, `f_written`, `f_past_valid` (see Design §1 for code)
  - SP1: `always_comb if (f_past_valid && f_written && addr == f_addr && !we) assert (rdata == f_shadow);`
  - SP2: Read stability — `$stable(addr) && !we && !$past(we)` → `$stable(rdata)`
  - SP3: Write isolation — `$past(we) && $past(addr) != f_addr` → `f_shadow == $past(f_shadow)`
  - SP4: Read-first — `$past(we && addr == f_addr && f_written)` → `rdata == $past(f_shadow)`
  - SP5: Cover — write then read back
- [ ] Run `make formal_sp` — all assertions PASS, cover trace generated
- [ ] Run `make sim_top` — verify simulation still passes

### Task 3: MAC unit formal properties (MAC1-MAC10)

**Files:** `rtl/mac_unit.sv`, `formal/mac_unit.sby`

- [ ] Create `formal/mac_unit.sby` with `[tasks] depth1 depth2 depth3` and `chparam` per task (see Design §2 for template). BMC depth 8.
- [ ] Add `ifdef FORMAL` block to `rtl/mac_unit.sv` (before `endmodule`):
  - Common infrastructure: `f_past_valid`, reset assumption
  - Tracking: `f_past_a`, `f_enable_count`
  - MAC1: Weight latch — `$past(rst_n && enable && load_weight)` → `weight_reg == $past(b)`
  - MAC2: Weight stability — `$past(rst_n && !(enable && load_weight))` → `$stable(weight_reg)`
  - MAC3: Reset clears — `rst_n && !$past(rst_n)` → all registers 0
  - MAC4: Activation fwd — `$past(rst_n && enable)` → `a_out == $past(a)`
  - MAC5: Weight fwd — `$past(rst_n && enable)` → `b_out == $past(b)`
  - MAC6: Stall hold — `$past(rst_n && !enable)` → `$stable(a_out) && $stable(b_out)`
  - MAC7: Depth 1 compute — `$past(enable)` → `psum_out == $past(psum_in + signed_ext(a) * signed_ext(weight_reg))`
  - MAC8: Depth 2 compute — track `f_past_mult_reg`, after 2+ enabled cycles assert pipeline correctness
  - MAC9: Depth 3 compute — track `f_past_mult_reg` and `f_past_add_reg`, after 3+ enabled cycles assert pipeline correctness
  - MAC10: Cover — nonzero inputs producing nonzero output
  - All assertions gated on `f_past_valid && rst_n && $past(rst_n)`
- [ ] Run `make formal_mac` — all 3 tasks PASS (depth1, depth2, depth3)
- [ ] Run `make sim_mac` — verify simulation still passes

### Task 4: Systolic array formal properties (ARR1-ARR7)

**Files:** `rtl/systolic_array.sv`, `formal/systolic_array.sby`

- [ ] Create `formal/systolic_array.sby` with multi-file read (mac_unit.sv + systolic_array.sv). BMC depth 6.

```
[tasks]
bmc

[options]
bmc: mode bmc
bmc: depth 6

[engines]
smtbmc yices

[script]
read -formal -sv rtl/mac_unit.sv
read -formal -sv rtl/systolic_array.sv
prep -top systolic_array

[files]
rtl/mac_unit.sv
rtl/systolic_array.sv
```

- [ ] Add `ifdef FORMAL` block to `rtl/systolic_array.sv` (before `endmodule`):
  - Common infrastructure: `f_past_valid`, reset assumption
  - ARR1: `assert (a_wire[0][0] == a_in[0 +: DATA_WIDTH]);` (combinational)
  - ARR2: For each row k (1 to ROWS-1), declare a formal-only k-deep shift register `f_skew_delay[k]` that tracks `a_in[k*DATA_WIDTH +: DATA_WIDTH]` delayed by k cycles. Assert `a_wire[k][0] == f_skew_delay[k]` when pipeline is full (`f_enable_count >= k`).
  - ARR3: Skew stall — when `!enable`, `$stable(a_wire[k][0])` for all k > 0
  - ARR4: `assert (psum_wire[0][j] == '0);` for all j (combinational)
  - ARR5: `assert (b_wire[0][j] == b_in[j*DATA_WIDTH +: DATA_WIDTH]);` for all j (combinational)
  - ARR6: `assert (drain_out[j*ACC_WIDTH +: ACC_WIDTH] == psum_wire[ROWS][j]);` for all j (combinational)
  - ARR7: Cover — `a_wire[ROWS-1][0] != '0`
- [ ] Run `make formal_array` — PASS
- [ ] Run `make sim_array` — verify simulation still passes

### Task 5: Controller formal properties (CTRL1-CTRL24)

**Files:** `rtl/controller.sv`, `formal/controller.sby`

This is the largest task. Properties are added incrementally within the single `ifdef FORMAL` block.

- [ ] Create `formal/controller.sby`:

```
[tasks]
bmc
cover
prove

[options]
bmc: mode bmc
bmc: depth 30
cover: mode cover
cover: depth 100
prove: mode prove
prove: depth 30

[engines]
smtbmc yices

[script]
read -formal -sv rtl/controller.sv
prep -top controller

[files]
rtl/controller.sv
```

- [ ] Add `ifdef FORMAL` block to `rtl/controller.sv` (before `endmodule`):

  **Common infrastructure:**
  - `f_past_valid`, reset assumption
  - Assume constraints: `dim_m`, `dim_k`, `dim_n` nonzero multiples of ROWS (use `assume` properties)
  - Assume `start` is single-cycle pulse: `assume property (start |=> !start)`

  **FSM safety (CTRL1-CTRL4):**
  - CTRL1: `assert (state <= S_DONE);`
  - CTRL2: Assert valid predecessor for each state using `$past(state)` — e.g., `state == S_FEED` implies `$past(state) == S_LOAD_WEIGHTS || $past(state) == S_FEED || $past(state) == S_WRITEBACK`
  - CTRL3: `assert property (state == S_DONE && f_past_valid && rst_n |=> state == S_DONE);`
  - CTRL4: `if (f_past_valid && rst_n && state != $past(state)) assert (phase_cnt == '0);`

  **Liveness (CTRL5-CTRL6):**
  - CTRL5: Assert phase_cnt upper bound per state using case statement
  - CTRL6: `cover property (state == S_DONE && mt_max == 1 && nt_max == 1 && kt_max == 1);`

  **Address sequencing (CTRL7a-CTRL14):**
  - Each property is a conditional assertion gated on the relevant state and phase_cnt value
  - CTRL7a: `if (state == S_IDLE && start) assert (sp_b_addr == ($clog2(SP_B_DEPTH))'(ROWS - 1));`
  - CTRL7b: `if (state == S_WRITEBACK && phase_cnt == wb_last && next_needs_load && more_tiles) assert (sp_b_addr == next_b_base + ($clog2(SP_B_DEPTH))'(ROWS - 1));`
  - CTRL9b: `if (state == S_WRITEBACK && phase_cnt == wb_last && !next_needs_load && more_tiles) assert (sp_a_addr == next_a_base);`
  - CTRL12c: `if (state == S_DRAIN && phase_cnt == 16'(DRAIN_CYCLES - 1) && wb_is_rmw) assert (sp_c_addr == c_base);`
  - (Similar pattern for CTRL8, CTRL9a, CTRL10, CTRL11, CTRL12a, CTRL12b, CTRL13, CTRL14)

  **Drain capture (CTRL15):**
  - Use `(* anyconst *)` formal-only `f_drain_m` and `f_drain_j` to select an arbitrary (m,j) pair within bounds, then assert `drain_regs[f_drain_m][f_drain_j]` is written exactly when expected

  **Tile loop (CTRL16-CTRL17):**
  - CTRL16: On WRITEBACK-to-next-state transition, assert next tile indices match the expected loop order (compare against `next_mt`, `next_kt`, `next_nt` which are already computed in the RTL)
  - CTRL17: `assert (next_needs_load == !(mt + 1'b1 < mt_max));`

  **Array control (CTRL18-CTRL20):**
  - CTRL18: `if (state != S_LOAD_WEIGHTS) assert (!arr_load_weight);`
  - CTRL19: `assert (arr_enable == (state == S_LOAD_WEIGHTS || state == S_FEED || state == S_DRAIN));`
  - CTRL20: `if (state == S_FEED) assert (arr_a_in == sp_a_rdata);` and `if (state == S_LOAD_WEIGHTS) assert (arr_b_in == sp_b_rdata);`

  **Cover traces (CTRL21-CTRL24):**
  - CTRL21: `cover property (state == S_DONE);` (with single-tile dimensions assumed)
  - CTRL22: `cover property (state == S_DONE && mt_max >= 2);`
  - CTRL23: `cover property (state == S_WRITEBACK && kt != '0);`
  - CTRL24: `cover property ($past(state) == S_WRITEBACK && state == S_FEED);`

- [ ] Run `make formal_ctrl` — bmc PASS, cover traces generated, prove PASS (or identify auxiliary invariants needed)
- [ ] Run `make sim_top` — verify simulation still passes

### Task 6: Top-level formal properties (TOP1-TOP14)

**Files:** `rtl/top.sv`, `formal/top.sby`

- [ ] Create `formal/top.sby` with reduced parameters (ROWS=2, COLS=2, SP_*_DEPTH=4, EXT_ADDR_W=2). BMC depth 10.

```
[tasks]
bmc
prove

[options]
bmc: mode bmc
bmc: depth 10
prove: mode prove
prove: depth 10

[engines]
smtbmc yices

[script]
read -formal -sv rtl/scratchpad.sv
read -formal -sv rtl/mac_unit.sv
read -formal -sv rtl/systolic_array.sv
read -formal -sv rtl/controller.sv
read -formal -sv rtl/top.sv
chparam -set ROWS 2 top
chparam -set COLS 2 top
chparam -set SP_A_DEPTH 4 top
chparam -set SP_B_DEPTH 4 top
chparam -set SP_C_DEPTH 4 top
chparam -set EXT_ADDR_W 2 top
prep -top top

[files]
rtl/scratchpad.sv
rtl/mac_unit.sv
rtl/systolic_array.sv
rtl/controller.sv
rtl/top.sv
```

- [ ] Add `ifdef FORMAL` block to `rtl/top.sv` (before `endmodule`):

  **Common infrastructure:**
  - `f_past_valid`, reset assumption
  - Assume constraints: valid dimensions (same as controller), `ext_sel` in {0,1,2}, `start` single-cycle pulse

  **Bank latching (TOP1-TOP3):**
  - TOP1: `if (f_past_valid && rst_n && $past(start && !ctrl_running_reg)) assert (active_bank_reg == $past(bank_sel));`
  - TOP2: `if (f_past_valid && rst_n && $past(rst_n) && ctrl_running_reg && !done) assert ($stable(active_bank_reg));`
  - TOP3: `if (start && !ctrl_running_reg) assert (active_bank == bank_sel);`

  **Mutual exclusion (TOP4-TOP6):**
  - TOP4: For each bank, assert controller we and host we are not both high. For SP_A bank 0: `assert (!(ctrl_running && active_bank == 1'b0 && sp_a0_we));` (since controller never writes A, sp_a0_we is only from host — but the property structure covers the general case)
  - TOP5: During `ctrl_running`, assert active bank addr == ctrl addr, non-active bank addr == ext addr or default
  - TOP6: Assert exactly one source drives each bank (use the three-way mux structure: controller branch, host branch, default branch — exactly one condition is true per bank)

  **Controller isolation (TOP7-TOP8):**
  - TOP7: `assert (ctrl_a_rdata == (active_bank ? sp_a1_rdata : sp_a0_rdata));` (same for B, C)
  - TOP8: Assert ext_rdata matches ext_sel mux of ext_bank-selected rdata

  **Host protection (TOP9-TOP10):**
  - TOP9: `if (ctrl_running && ext_bank == active_bank) assert (sp_a{active_bank}_we == 1'b0 || sp_a{active_bank}_we comes from controller);` — verify host ext_we doesn't propagate to active bank. Concretely, when `ctrl_running && active_bank == 0`, assert `sp_a0_we` is not influenced by `ext_we`.
  - TOP10: When `ctrl_running && ext_bank != active_bank`, assert inactive bank addr/we/wdata match host signals

  **Default tie-off (TOP11):**
  - For each bank, assert that when neither controller nor host selects it, `we == 0 && addr == 0`

  **ctrl_running (TOP12-TOP13):**
  - TOP12: `if (f_past_valid && rst_n && $past(rst_n && start && !ctrl_running_reg)) assert (ctrl_running_reg);`
  - TOP13: `if (start) assert (ctrl_running);`

  **Cover (TOP14):**
  - `cover property (f_past_valid && done && $past(!done) && active_bank == 1'b0);` followed by observing start with bank_sel=1

- [ ] Run `make formal_top` — bmc PASS, prove PASS
- [ ] Run `make sim_top` — verify simulation still passes

### Task 7: Final validation

- [ ] Run `make formal` — all 5 modules PASS (bmc + prove + cover)
- [ ] Run `make sim_mac && make sim_array && make sim_top` — all simulation tests still pass
- [ ] Review cover trace VCDs (in `formal/*/engine_0/trace*.vcd`) for sanity — verify the solver found meaningful traces, not degenerate ones
- [ ] Verify no RTL logic was changed (only `ifdef FORMAL` additions): `git diff rtl/*.sv` should show only additions within `ifdef FORMAL` blocks
