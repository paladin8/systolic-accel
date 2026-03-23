# Milestone 1: MAC Unit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a parameterized, 2-stage pipelined multiply-accumulate unit with registered passthrough — the fundamental building block of the systolic array.

**Architecture:** The MAC unit has two pipeline stages: stage 1 registers the product `a * b` into `mult_reg` and passes `a`/`b` through to neighbors; stage 2 accumulates `mult_reg` into `acc_reg` (or replaces it on `clear_acc`). Total latency from input to accumulated result: 2 clock cycles.

**Tech Stack:** SystemVerilog (Verilator 5.020 for simulation), C++ testbench, GNU Make

**Spec reference:** `.ai/OVERALL_DESIGN.md` lines 84–141

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `Makefile` | Create | Build targets for Verilator simulation |
| `rtl/mac_unit.sv` | Create | 2-stage pipelined MAC with registered passthrough |
| `tb/tb_mac_unit.cpp` | Create | Verilator C++ testbench — 7 test cases, VCD dump |

---

### Task 1: Makefile

**Files:**
- Create: `Makefile`

- [ ] **Step 1: Write the Makefile**

```makefile
.PHONY: sim_mac clean

sim_mac:
	verilator --cc --exe --build --trace \
		-Wall -Wno-fatal \
		rtl/mac_unit.sv tb/tb_mac_unit.cpp \
		-o mac_sim
	./obj_dir/mac_sim

clean:
	rm -rf obj_dir waves/*.vcd
```

- [ ] **Step 2: Commit**

```bash
git add Makefile
git commit -m "add Makefile with sim_mac target"
```

---

### Task 2: MAC Unit RTL

**Files:**
- Create: `rtl/mac_unit.sv`

The module interface is defined in the spec. Key details:
- Parameters: `DATA_WIDTH=16`, `ACC_WIDTH=32`
- 9 ports: `clk`, `rst_n`, `enable`, `clear_acc`, `a`, `b`, `a_out`, `b_out`, `result`
- Two `always_ff` blocks: one for stage 1 (multiply + passthrough), one for stage 2 (accumulate)
- All registers reset to zero on `!rst_n`
- All registers gated by `enable`

- [ ] **Step 1: Write `rtl/mac_unit.sv`**

```systemverilog
module mac_unit #(
    parameter DATA_WIDTH = 16,
    parameter ACC_WIDTH  = 32
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  enable,
    input  logic                  clear_acc,
    input  logic [DATA_WIDTH-1:0] a,
    input  logic [DATA_WIDTH-1:0] b,
    output logic [DATA_WIDTH-1:0] a_out,
    output logic [DATA_WIDTH-1:0] b_out,
    output logic [ACC_WIDTH-1:0]  result
);

    logic [ACC_WIDTH-1:0] mult_reg;
    logic [ACC_WIDTH-1:0] acc_reg;

    // Stage 1: registered multiplier output + passthrough (3 registers: mult_reg, a_out, b_out)
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mult_reg <= '0;
            a_out    <= '0;
            b_out    <= '0;
        end else if (enable) begin
            mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(b));
            a_out    <= a;
            b_out    <= b;
        end
    end

    // Stage 2: accumulator register (1 register: acc_reg)
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc_reg <= '0;
        end else if (enable) begin
            if (clear_acc)
                acc_reg <= mult_reg;
            else
                acc_reg <= acc_reg + mult_reg;
        end
    end

    assign result = acc_reg;

endmodule
```

Key design decisions:
- Signed multiplication: `$signed()` casts make operands signed before widening to `ACC_WIDTH`, matching the spec's `int16 × int16 → int32` semantics. Uses `$signed()` over `signed'()` for broader Verilator compatibility.
- `clear_acc` replaces (not zeroes) the accumulator — it loads `mult_reg` so the current product isn't lost.
- Async reset on `rst_n` (standard for ASIC-style design with Verilator).

- [ ] **Step 2: Verify it compiles**

Run: `verilator --cc --trace -Wall -Wno-fatal rtl/mac_unit.sv`
Expected: Clean compilation, `obj_dir/` populated with generated C++ files.

- [ ] **Step 3: Commit**

```bash
git add rtl/mac_unit.sv
git commit -m "add MAC unit RTL — 2-stage pipelined multiply-accumulate"
```

---

### Task 3: Testbench Scaffold + Passthrough Timing Test

**Files:**
- Create: `tb/tb_mac_unit.cpp`

The testbench needs:
- Verilator includes and model header
- Clock generation helper
- Reset sequence
- VCD trace setup (dump to `waves/mac_unit.vcd`)
- Test pass/fail reporting with exit code

- [ ] **Step 1: Write testbench scaffold with passthrough timing test**

```cpp
#include <cstdio>
#include <cstdlib>
#include "Vmac_unit.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

static vluint64_t sim_time = 0;
static int test_failures = 0;

#define CHECK(cond, fmt, ...) \
    do { if (!(cond)) { printf("FAIL [%lu]: " fmt "\n", sim_time, ##__VA_ARGS__); test_failures++; } } while(0)

void tick(Vmac_unit* dut, VerilatedVcdC* tfp) {
    dut->clk = 0;
    dut->eval();
    tfp->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    tfp->dump(sim_time++);
}

void reset(Vmac_unit* dut, VerilatedVcdC* tfp) {
    dut->rst_n = 0;
    dut->enable = 0;
    dut->clear_acc = 0;
    dut->a = 0;
    dut->b = 0;
    tick(dut, tfp);
    tick(dut, tfp);
    dut->rst_n = 1;
    tick(dut, tfp);
}

// Test 1: a_out and b_out appear exactly 1 cycle after a and b
void test_passthrough_timing(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: passthrough timing\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->a = 42;
    dut->b = 7;

    // Before the clock edge, outputs should still be 0 (from reset)
    CHECK(dut->a_out == 0, "a_out should be 0 before first tick");
    CHECK(dut->b_out == 0, "b_out should be 0 before first tick");

    tick(dut, tfp);

    // After 1 cycle, a_out and b_out should reflect the inputs
    CHECK(dut->a_out == 42, "a_out should be 42, got %d", dut->a_out);
    CHECK(dut->b_out == 7,  "b_out should be 7, got %d", dut->b_out);

    // Change inputs and verify old values persist until next tick
    dut->a = 100;
    dut->b = 200;
    // a_out/b_out still hold previous values (registered)
    CHECK(dut->a_out == 42, "a_out should still be 42 before tick");

    tick(dut, tfp);
    CHECK(dut->a_out == 100, "a_out should be 100, got %d", dut->a_out);
    CHECK(dut->b_out == 200, "b_out should be 200, got %d", dut->b_out);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vmac_unit* dut = new Vmac_unit;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    system("mkdir -p waves");
    tfp->open("waves/mac_unit.vcd");

    test_passthrough_timing(dut, tfp);

    tfp->close();
    delete tfp;
    delete dut;

    if (test_failures == 0) {
        printf("\nAll tests PASSED\n");
    } else {
        printf("\n%d test(s) FAILED\n", test_failures);
    }
    return test_failures ? 1 : 0;
}
```

- [ ] **Step 2: Build and run**

Run: `make sim_mac`
Expected: Compiles cleanly, test passes, `waves/mac_unit.vcd` created.

- [ ] **Step 3: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add MAC testbench with passthrough timing test"
```

---

### Task 4: Basic Accumulation Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

Feed 4 `(a, b)` pairs with `clear_acc=0`, wait for pipeline drain (2 cycles after last input), verify `result` equals the sum of products.

- [ ] **Step 1: Add accumulation test function**

Add before `main()`:

```cpp
// Test 2: feed 4 (a,b) pairs, verify sum-of-products
void test_basic_accumulation(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: basic accumulation\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;  // Clear on first product

    // Pair 0: 3 * 4 = 12
    dut->a = 3; dut->b = 4;
    tick(dut, tfp);

    // After this tick: Stage 1 latched mult_reg = 12.
    // Stage 2 saw clear_acc=1 and loaded old mult_reg (0 from reset), so acc_reg = 0.
    dut->clear_acc = 0;  // Accumulate from now on

    // Pair 1: 5 * 6 = 30
    dut->a = 5; dut->b = 6;
    tick(dut, tfp);
    // Now: mult_reg = 30, acc_reg = 0 + 12 = 12 (via accumulation, clear_acc is now 0)

    // Pair 2: 2 * 7 = 14
    dut->a = 2; dut->b = 7;
    tick(dut, tfp);
    // Now: mult_reg = 14, acc_reg = 12 + 30 = 42

    // Pair 3: 1 * 8 = 8
    dut->a = 1; dut->b = 8;
    tick(dut, tfp);
    // Now: mult_reg = 8, acc_reg = 42 + 14 = 56

    // Drain: one more tick for last mult_reg to accumulate
    dut->a = 0; dut->b = 0;
    tick(dut, tfp);
    // Now: mult_reg = 0, acc_reg = 56 + 8 = 64

    // One more tick so the zero product doesn't pollute — actually we need to
    // stop enable or accept that acc will add 0. Adding 0 is fine.
    tick(dut, tfp);
    // acc_reg = 64 + 0 = 64

    // Expected: 3*4 + 5*6 + 2*7 + 1*8 = 12 + 30 + 14 + 8 = 64
    CHECK(dut->result == 64, "accumulation expected 64, got %d", dut->result);
}
```

- [ ] **Step 2: Add call in `main()` before `tfp->close()`**

```cpp
    test_basic_accumulation(dut, tfp);
```

- [ ] **Step 3: Build and run**

Run: `make sim_mac`
Expected: Both tests pass.

- [ ] **Step 4: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add basic accumulation test for MAC unit"
```

---

### Task 5: Reset Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

Assert `rst_n` low mid-accumulation, verify all registers clear to zero.

- [ ] **Step 1: Add reset test function**

Add before `main()`:

```cpp
// Test 3: assert rst_n low, verify accumulator and passthrough clear to zero
void test_reset(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: reset behavior\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Feed some values to get nonzero state
    dut->a = 10; dut->b = 20;
    tick(dut, tfp);          // Stage 1: mult_reg = 200. Stage 2: clear_acc=1, loads old mult_reg (0) → acc_reg = 0
    dut->clear_acc = 0;
    tick(dut, tfp);          // Stage 1: mult_reg = 200. Stage 2: acc_reg = 0 + 200 = 200
    CHECK(dut->result == 200, "pre-reset: expected 200, got %d", dut->result);

    // Assert reset
    dut->rst_n = 0;
    tick(dut, tfp);

    CHECK(dut->result == 0, "after reset: result should be 0, got %d", dut->result);
    CHECK(dut->a_out == 0,  "after reset: a_out should be 0, got %d", dut->a_out);
    CHECK(dut->b_out == 0,  "after reset: b_out should be 0, got %d", dut->b_out);

    // Release reset
    dut->rst_n = 1;
    tick(dut, tfp);
    CHECK(dut->result == 0, "after reset release: result should still be 0, got %d", dut->result);
}
```

- [ ] **Step 2: Add call in `main()`**

- [ ] **Step 3: Build and run**

Run: `make sim_mac`
Expected: All 3 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add reset behavior test for MAC unit"
```

---

### Task 6: Clear-Acc Mid-Stream Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

Accumulate some products, assert `clear_acc` between two sequences, verify the first sequence's result is discarded and only the second sequence's result remains.

- [ ] **Step 1: Add clear_acc test function**

Add before `main()`:

```cpp
// Test 4: clear_acc between two accumulation sequences
void test_clear_midstream(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: clear_acc mid-stream\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Sequence 1: 10*10 + 20*20 = 100 + 400 = 500
    dut->a = 10; dut->b = 10;
    tick(dut, tfp);          // mult_reg = 100
    dut->clear_acc = 0;
    dut->a = 20; dut->b = 20;
    tick(dut, tfp);          // mult_reg = 400, acc_reg = 100
    dut->a = 0; dut->b = 0;
    tick(dut, tfp);          // mult_reg = 0, acc_reg = 100 + 400 = 500
    tick(dut, tfp);          // acc_reg = 500 + 0 = 500
    CHECK(dut->result == 500, "seq1: expected 500, got %d", dut->result);

    // Assert clear_acc to start sequence 2
    dut->clear_acc = 1;
    dut->a = 3; dut->b = 3;
    tick(dut, tfp);          // Stage 1: mult_reg = 9. Stage 2: clear_acc=1, loads old mult_reg (0) → acc_reg = 0
    dut->clear_acc = 0;
    dut->a = 4; dut->b = 4;
    tick(dut, tfp);          // mult_reg = 16, acc_reg = 0 + 9 = 9
    dut->a = 0; dut->b = 0;
    tick(dut, tfp);          // mult_reg = 0, acc_reg = 9 + 16 = 25
    tick(dut, tfp);          // acc_reg = 25 + 0 = 25

    // Sequence 2 result: 3*3 + 4*4 = 9 + 16 = 25
    CHECK(dut->result == 25, "seq2: expected 25, got %d", dut->result);
}
```

- [ ] **Step 2: Add call in `main()`**

- [ ] **Step 3: Build and run**

Run: `make sim_mac`
Expected: All 4 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add clear_acc mid-stream test for MAC unit"
```

---

### Task 7: Boundary / Overflow Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

Feed max-value signed 16-bit inputs and verify the 32-bit accumulator handles the product correctly.

- [ ] **Step 1: Add boundary test function**

Add before `main()`:

```cpp
// Test 5: max-value inputs, verify signed multiplication and accumulator width
void test_boundary(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: boundary values\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Max positive * max positive: 32767 * 32767 = 1,073,676,289
    // This fits in int32 (max 2,147,483,647)
    dut->a = 0x7FFF;  // 32767 as signed 16-bit
    dut->b = 0x7FFF;
    tick(dut, tfp);    // Stage 1: mult_reg = 1,073,676,289. Stage 2: clear_acc=1, loads old mult_reg (0) → acc_reg = 0
    dut->clear_acc = 0;

    // Max negative * max positive: -32768 * 32767 = -1,073,709,056
    dut->a = 0x8000;  // -32768 as signed 16-bit
    dut->b = 0x7FFF;  // 32767
    tick(dut, tfp);    // acc_reg = 0 + 1,073,676,289 = 1,073,676,289

    dut->a = 0; dut->b = 0;
    tick(dut, tfp);    // acc_reg = 1,073,676,289 + (-1,073,709,056) = -32,767
    tick(dut, tfp);    // drain

    // 32767*32767 + (-32768)*32767 = 32767*(32767 - 32768) = 32767*(-1) = -32767
    int32_t expected = -32767;
    int32_t got = (int32_t)dut->result;
    CHECK(got == expected, "boundary: expected %d, got %d", expected, got);
}
```

- [ ] **Step 2: Add call in `main()`**

- [ ] **Step 3: Build and run**

Run: `make sim_mac`
Expected: All 5 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add boundary value test for MAC unit"
```

---

### Task 8: Enable-Stall Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

Verify that when `enable=0`, all registers hold their values — the pipeline freezes completely.

- [ ] **Step 1: Add enable-stall test function**

Add before `main()`:

```cpp
// Test 6: enable=0 freezes all pipeline registers
void test_enable_stall(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: enable stall\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Get some nonzero state: feed 5*6, let it accumulate
    dut->a = 5; dut->b = 6;
    tick(dut, tfp);          // mult_reg = 30, acc_reg = 0 (clear loaded old mult_reg=0)
    dut->clear_acc = 0;
    tick(dut, tfp);          // mult_reg = 30, acc_reg = 0 + 30 = 30

    // Freeze the pipeline
    dut->enable = 0;
    dut->a = 99; dut->b = 99;  // These should be ignored

    tick(dut, tfp);
    tick(dut, tfp);
    tick(dut, tfp);

    // Everything should still hold the pre-freeze values
    CHECK(dut->result == 30, "stall: result should still be 30, got %d", dut->result);
    CHECK(dut->a_out == 5,   "stall: a_out should still be 5, got %d", dut->a_out);
    CHECK(dut->b_out == 6,   "stall: b_out should still be 6, got %d", dut->b_out);

    // Resume and verify pipeline works again
    dut->enable = 1;
    dut->a = 2; dut->b = 3;
    tick(dut, tfp);          // mult_reg = 6, acc_reg = 30 + 30 = 60 (old mult_reg=30 still in pipeline)
    dut->a = 0; dut->b = 0;
    tick(dut, tfp);          // mult_reg = 0, acc_reg = 60 + 6 = 66
    tick(dut, tfp);          // drain: acc_reg = 66 + 0 = 66
    CHECK(dut->result == 66, "resume: expected 66, got %d", dut->result);
}
```

- [ ] **Step 2: Add call in `main()`**

- [ ] **Step 3: Build and run**

Run: `make sim_mac`
Expected: All 6 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add enable-stall test for MAC unit"
```

---

### Task 9: Negative Accumulation Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

Verify signed accumulation across multiple pairs with negative intermediate products.

- [ ] **Step 1: Add negative accumulation test function**

Add before `main()`:

```cpp
// Test 7: signed accumulation with negative products
void test_negative_accumulation(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: negative accumulation\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // (-3) * 4 = -12
    dut->a = (uint16_t)(int16_t)-3;  // 0xFFFD
    dut->b = 4;
    tick(dut, tfp);          // mult_reg = -12, acc_reg = 0 (clear loaded old mult_reg=0)
    dut->clear_acc = 0;

    // 5 * (-6) = -30
    dut->a = 5;
    dut->b = (uint16_t)(int16_t)-6;  // 0xFFFA
    tick(dut, tfp);          // mult_reg = -30, acc_reg = 0 + (-12) = -12

    // (-7) * (-8) = 56
    dut->a = (uint16_t)(int16_t)-7;  // 0xFFF9
    dut->b = (uint16_t)(int16_t)-8;  // 0xFFF8
    tick(dut, tfp);          // mult_reg = 56, acc_reg = -12 + (-30) = -42

    dut->a = 0; dut->b = 0;
    tick(dut, tfp);          // mult_reg = 0, acc_reg = -42 + 56 = 14
    tick(dut, tfp);          // drain: acc_reg = 14 + 0 = 14

    // Expected: (-3)*4 + 5*(-6) + (-7)*(-8) = -12 + (-30) + 56 = 14
    int32_t expected = 14;
    int32_t got = (int32_t)dut->result;
    CHECK(got == expected, "negative accum: expected %d, got %d", expected, got);
}
```

- [ ] **Step 2: Add call in `main()`**

- [ ] **Step 3: Build and run**

Run: `make sim_mac`
Expected: All 7 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tb/tb_mac_unit.cpp
git commit -m "add negative accumulation test for MAC unit"
```

---

### Task 10: Final Verification

- [ ] **Step 1: Run full test suite from clean state**

```bash
make clean && make sim_mac
```

Expected: All 7 tests pass, `waves/mac_unit.vcd` created.

- [ ] **Step 2: Inspect waveform shows correct pipeline timing**

Open `waves/mac_unit.vcd` (or verify file is non-empty):
```bash
ls -la waves/mac_unit.vcd
```

The waveform should show:
- `mult_reg` updates 1 cycle after `a`/`b` change
- `acc_reg` updates 1 cycle after `mult_reg` changes
- `a_out`/`b_out` update on the same cycle as `mult_reg` (both stage 1)

- [ ] **Step 3: Final commit (if any cleanup needed)**

Milestone 1 is complete when all 7 test cases pass and the VCD waveform confirms 2-stage pipeline timing.
