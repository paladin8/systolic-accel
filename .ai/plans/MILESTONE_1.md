# Milestone 1: MAC Unit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a parameterized, 2-stage pipelined MAC unit for weight-stationary systolic dataflow — the fundamental building block of the systolic array. Each MAC holds a pre-loaded weight, multiplies it by streaming activations, and forwards partial sums vertically.

**Architecture:** The MAC unit has a weight register (loaded once, stationary during compute) and two pipeline stages: stage 1 registers the product `a * weight_reg` into `mult_reg` and passes `a`/`b` through to neighbors; stage 2 adds `mult_reg` to the incoming partial sum `psum_in` and outputs `psum_out`. Total latency from activation input to partial sum output: 2 clock cycles.

**Tech Stack:** SystemVerilog (Verilator 5.020 for simulation), C++ testbench, GNU Make

**Spec reference:** `.ai/OVERALL_DESIGN.md` M1 section

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `Makefile` | Create | Build targets for Verilator simulation |
| `rtl/mac_unit.sv` | Create | 2-stage pipelined MAC with weight register and partial sum flow |
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

---

### Task 2: MAC Unit RTL

**Files:**
- Create: `rtl/mac_unit.sv`

The module interface is defined in the spec. Key details:
- Parameters: `DATA_WIDTH=16`, `ACC_WIDTH=32`
- 11 ports: `clk`, `rst_n`, `enable`, `load_weight`, `a`, `b`, `a_out`, `b_out`, `psum_in`, `psum_out`
- Weight register: loaded when `load_weight && enable`, holds otherwise
- Stage 1 `always_ff`: multiply `a * weight_reg` into `mult_reg`, register `a` → `a_out` and `b` → `b_out`
- Stage 2 `always_ff`: `psum_out <= psum_in + mult_reg`
- All registers reset to zero on `!rst_n`, gated by `enable`

- [ ] **Step 1: Write `rtl/mac_unit.sv`**

```systemverilog
module mac_unit #(
    parameter DATA_WIDTH = 16,
    parameter ACC_WIDTH  = 32
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  enable,
    input  logic                  load_weight,
    input  logic [DATA_WIDTH-1:0] a,
    input  logic [DATA_WIDTH-1:0] b,
    output logic [DATA_WIDTH-1:0] a_out,
    output logic [DATA_WIDTH-1:0] b_out,
    input  logic [ACC_WIDTH-1:0]  psum_in,
    output logic [ACC_WIDTH-1:0]  psum_out
);

    logic [DATA_WIDTH-1:0] weight_reg;
    logic [ACC_WIDTH-1:0]  mult_reg;

    // Weight register — loaded during weight phase, stationary during compute
    // Synthesizes to: weight_reg[DATA_WIDTH] flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            weight_reg <= '0;
        else if (enable && load_weight)
            weight_reg <= b;
    end

    // Stage 1: registered multiplier output + passthrough
    // Synthesizes to: mult_reg[ACC_WIDTH] + a_out[DATA_WIDTH] + b_out[DATA_WIDTH] flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mult_reg <= '0;
            a_out    <= '0;
            b_out    <= '0;
        end else if (enable) begin
            mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            a_out    <= a;
            b_out    <= b;
        end
    end

    // Stage 2: partial sum addition
    // Synthesizes to: psum_out[ACC_WIDTH] flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            psum_out <= '0;
        else if (enable)
            psum_out <= psum_in + mult_reg;
    end

endmodule
```

Key design decisions:
- Weight register has separate load condition (`enable && load_weight`), not gated by stage 1. Weight stays fixed when `load_weight=0`.
- Signed multiplication: `$signed()` casts for sign-extension before widening to `ACC_WIDTH`.
- `b_out` always passes `b` through (used during weight loading to shift weights down the column).
- `psum_out = psum_in + mult_reg` — no accumulator. Partial sums flow through, not stored.
- Async reset on `rst_n` (ASIC convention).

- [ ] **Step 2: Verify it compiles**

Run: `verilator --cc --trace -Wall -Wno-fatal rtl/mac_unit.sv`
Expected: Clean compilation, `obj_dir/` populated with generated C++ files.

---

### Task 3: Testbench Scaffold + Weight Loading Test

**Files:**
- Create: `tb/tb_mac_unit.cpp`

The testbench needs:
- Verilator includes and model header
- Clock generation helper
- Reset sequence
- VCD trace setup (dump to `waves/mac_unit.vcd`)
- Test pass/fail reporting with exit code

- [ ] **Step 1: Write testbench scaffold with weight loading test**

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
    dut->load_weight = 0;
    dut->a = 0;
    dut->b = 0;
    dut->psum_in = 0;
    tick(dut, tfp);
    tick(dut, tfp);
    dut->rst_n = 1;
    tick(dut, tfp);
}

// Test 1: load a weight, verify it persists during compute
void test_weight_loading(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: weight loading\n");
    reset(dut, tfp);

    // Load weight = 42
    dut->enable = 1;
    dut->load_weight = 1;
    dut->b = 42;
    tick(dut, tfp);

    // Switch to compute mode, change b — weight should persist
    dut->load_weight = 0;
    dut->b = 99;  // Should be ignored by weight_reg
    dut->a = 3;
    tick(dut, tfp);
    // Stage 1: mult_reg = 3 * 42 = 126

    dut->a = 0;
    tick(dut, tfp);
    // Stage 2: psum_out = 0 + 126 = 126

    CHECK(dut->psum_out == 126, "weight should persist: expected psum_out=126, got %d", dut->psum_out);

    // Feed another activation — weight should still be 42
    dut->a = 2;
    tick(dut, tfp);
    // Stage 1: mult_reg = 2 * 42 = 84
    tick(dut, tfp);
    // Stage 2: psum_out = 0 + 84 = 84 (psum_in is still 0)

    CHECK(dut->psum_out == 84, "weight still 42: expected psum_out=84, got %d", dut->psum_out);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vmac_unit* dut = new Vmac_unit;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    if (system("mkdir -p waves") != 0) {
        fprintf(stderr, "Warning: could not create waves/ directory\n");
    }
    tfp->open("waves/mac_unit.vcd");

    test_weight_loading(dut, tfp);

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

---

### Task 4: Multiply + Passthrough Timing Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [ ] **Step 1: Add passthrough timing test**

```cpp
// Test 2: a_out appears 1 cycle after a, mult_reg uses stored weight
void test_passthrough_timing(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: passthrough timing\n");
    reset(dut, tfp);

    // Load weight = 5
    dut->enable = 1;
    dut->load_weight = 1;
    dut->b = 5;
    tick(dut, tfp);
    dut->load_weight = 0;

    // Feed activation = 7
    dut->a = 7;
    CHECK(dut->a_out == 0, "a_out should be 0 before tick");
    tick(dut, tfp);
    CHECK(dut->a_out == 7, "a_out should be 7, got %d", dut->a_out);

    // Change activation, verify old a_out persists until next tick
    dut->a = 100;
    CHECK(dut->a_out == 7, "a_out should still be 7 before tick");
    tick(dut, tfp);
    CHECK(dut->a_out == 100, "a_out should be 100, got %d", dut->a_out);
}
```

- [ ] **Step 2: Add call in `main()`, build and run**

---

### Task 5: Partial Sum Chain Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [ ] **Step 1: Add partial sum chain test**

```cpp
// Test 3: psum_out = psum_in + a * weight, with varying psum_in
void test_partial_sum_chain(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: partial sum chain\n");
    reset(dut, tfp);

    // Load weight = 10
    dut->enable = 1;
    dut->load_weight = 1;
    dut->b = 10;
    tick(dut, tfp);
    dut->load_weight = 0;

    // Feed a=3 with psum_in=100
    dut->a = 3;
    dut->psum_in = 100;
    tick(dut, tfp);
    // Stage 1: mult_reg = 3*10 = 30

    tick(dut, tfp);
    // Stage 2: psum_out = 100 + 30 = 130
    // Note: psum_in is still 100 at this point

    CHECK(dut->psum_out == 130, "psum chain: expected 130, got %d", dut->psum_out);

    // Change psum_in to simulate different partial sum from above
    dut->a = 5;
    dut->psum_in = 200;
    tick(dut, tfp);
    // Stage 1: mult_reg = 5*10 = 50

    tick(dut, tfp);
    // Stage 2: psum_out = 200 + 50 = 250

    CHECK(dut->psum_out == 250, "psum chain: expected 250, got %d", dut->psum_out);
}
```

- [ ] **Step 2: Add call in `main()`, build and run**

---

### Task 6: Weight Loading Chain (b_out) Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [ ] **Step 1: Add weight loading chain test**

```cpp
// Test 4: b_out passes weights through for column loading
void test_weight_chain(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: weight loading chain\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->load_weight = 1;

    // Feed weight value through b, verify b_out passes it 1 cycle later
    dut->b = 55;
    tick(dut, tfp);
    CHECK(dut->b_out == 55, "b_out should pass 55, got %d", dut->b_out);

    // Next weight value
    dut->b = 77;
    tick(dut, tfp);
    CHECK(dut->b_out == 77, "b_out should pass 77, got %d", dut->b_out);

    // After loading, b_out should still pass through (but b_in will be 0 during compute)
    dut->load_weight = 0;
    dut->b = 0;
    tick(dut, tfp);
    CHECK(dut->b_out == 0, "b_out should be 0 during compute, got %d", dut->b_out);
}
```

- [ ] **Step 2: Add call in `main()`, build and run**

---

### Task 7: Reset Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [ ] **Step 1: Add reset test**

```cpp
// Test 5: assert rst_n low, verify all registers clear to zero
void test_reset(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: reset behavior\n");
    reset(dut, tfp);

    // Build up nonzero state
    dut->enable = 1;
    dut->load_weight = 1;
    dut->b = 10;
    tick(dut, tfp);
    dut->load_weight = 0;
    dut->a = 5;
    dut->psum_in = 100;
    tick(dut, tfp);
    tick(dut, tfp);

    // Assert reset
    dut->rst_n = 0;
    tick(dut, tfp);

    CHECK(dut->psum_out == 0, "after reset: psum_out should be 0, got %d", dut->psum_out);
    CHECK(dut->a_out == 0, "after reset: a_out should be 0, got %d", dut->a_out);
    CHECK(dut->b_out == 0, "after reset: b_out should be 0, got %d", dut->b_out);

    // Release reset, verify weight_reg is also cleared
    dut->rst_n = 1;
    dut->a = 7;
    dut->psum_in = 0;
    tick(dut, tfp);
    // mult_reg = 7 * 0 (weight_reg cleared) = 0
    tick(dut, tfp);
    CHECK(dut->psum_out == 0, "after reset: weight should be 0, psum_out should be 0, got %d", dut->psum_out);
}
```

- [ ] **Step 2: Add call in `main()`, build and run**

---

### Task 8: Enable Stall Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [ ] **Step 1: Add enable stall test**

```cpp
// Test 6: enable=0 freezes all registers including weight_reg
void test_enable_stall(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: enable stall\n");
    reset(dut, tfp);

    // Load weight and build state
    dut->enable = 1;
    dut->load_weight = 1;
    dut->b = 8;
    tick(dut, tfp);
    dut->load_weight = 0;
    dut->a = 3;
    dut->psum_in = 50;
    tick(dut, tfp);
    // mult_reg = 3*8 = 24
    tick(dut, tfp);
    // psum_out = 50 + 24 = 74

    CHECK(dut->psum_out == 74, "pre-stall: expected 74, got %d", dut->psum_out);

    // Freeze pipeline
    dut->enable = 0;
    dut->a = 99;
    dut->psum_in = 999;
    tick(dut, tfp);
    tick(dut, tfp);
    tick(dut, tfp);

    CHECK(dut->psum_out == 74, "stall: psum_out should still be 74, got %d", dut->psum_out);
    CHECK(dut->a_out == 3, "stall: a_out should still be 3, got %d", dut->a_out);

    // Resume
    dut->enable = 1;
    dut->a = 2;
    dut->psum_in = 0;
    tick(dut, tfp);
    // mult_reg = 2*8 = 16 (weight still 8)
    tick(dut, tfp);
    // psum_out = 0 + 16 = 16

    CHECK(dut->psum_out == 16, "resume: expected 16, got %d", dut->psum_out);
}
```

- [ ] **Step 2: Add call in `main()`, build and run**

---

### Task 9: Signed Values Test

**Files:**
- Modify: `tb/tb_mac_unit.cpp`

- [ ] **Step 1: Add signed values test**

```cpp
// Test 7: negative activations and weights
void test_signed_values(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: signed values\n");
    reset(dut, tfp);

    // Load negative weight: -5
    dut->enable = 1;
    dut->load_weight = 1;
    dut->b = (uint16_t)(int16_t)-5;  // 0xFFFB
    tick(dut, tfp);
    dut->load_weight = 0;

    // Positive activation * negative weight: 10 * (-5) = -50
    dut->a = 10;
    dut->psum_in = 100;
    tick(dut, tfp);
    // mult_reg = -50
    tick(dut, tfp);
    // psum_out = 100 + (-50) = 50

    int32_t got = (int32_t)dut->psum_out;
    CHECK(got == 50, "signed: expected 50, got %d", got);

    // Negative activation * negative weight: (-3) * (-5) = 15
    dut->a = (uint16_t)(int16_t)-3;  // 0xFFFD
    dut->psum_in = 0;
    tick(dut, tfp);
    // mult_reg = 15
    tick(dut, tfp);
    // psum_out = 0 + 15 = 15

    got = (int32_t)dut->psum_out;
    CHECK(got == 15, "neg*neg: expected 15, got %d", got);
}
```

- [ ] **Step 2: Add call in `main()`, build and run**

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
- `weight_reg` latches on `load_weight` posedge, holds steady during compute
- `mult_reg` updates 1 cycle after activation arrives
- `psum_out` updates 1 cycle after `mult_reg` (= 2 cycles after activation)
- `a_out`/`b_out` update on the same cycle as `mult_reg` (both stage 1)

Milestone 1 is complete when all 7 test cases pass and the VCD waveform confirms weight-stationary 2-stage pipeline timing.
