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

// Test 2: feed 4 (a,b) pairs, verify sum-of-products
void test_basic_accumulation(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: basic accumulation\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Pair 0: 3 * 4 = 12
    dut->a = 3; dut->b = 4;
    tick(dut, tfp);
    // Stage 1 latched mult_reg = 12.
    // Stage 2 saw clear_acc=1 and loaded old mult_reg (0 from reset), so acc_reg = 0.
    dut->clear_acc = 0;

    // Pair 1: 5 * 6 = 30
    dut->a = 5; dut->b = 6;
    tick(dut, tfp);
    // mult_reg = 30, acc_reg = 0 + 12 = 12 (via accumulation, clear_acc is now 0)

    // Pair 2: 2 * 7 = 14
    dut->a = 2; dut->b = 7;
    tick(dut, tfp);
    // mult_reg = 14, acc_reg = 12 + 30 = 42

    // Pair 3: 1 * 8 = 8
    dut->a = 1; dut->b = 8;
    tick(dut, tfp);
    // mult_reg = 8, acc_reg = 42 + 14 = 56

    // Drain: one more tick for last mult_reg to accumulate
    dut->a = 0; dut->b = 0;
    tick(dut, tfp);
    // mult_reg = 0, acc_reg = 56 + 8 = 64

    tick(dut, tfp);
    // acc_reg = 64 + 0 = 64

    // Expected: 3*4 + 5*6 + 2*7 + 1*8 = 12 + 30 + 14 + 8 = 64
    CHECK(dut->result == 64, "accumulation expected 64, got %d", dut->result);
}

// Test 3: assert rst_n low, verify accumulator and passthrough clear to zero
void test_reset(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: reset behavior\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Feed some values to get nonzero state
    dut->a = 10; dut->b = 20;
    tick(dut, tfp);          // Stage 1: mult_reg = 200. Stage 2: clear_acc=1, loads old mult_reg (0) -> acc_reg = 0
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
    tick(dut, tfp);          // Stage 1: mult_reg = 9. Stage 2: clear_acc=1, loads old mult_reg (0) -> acc_reg = 0
    dut->clear_acc = 0;
    dut->a = 4; dut->b = 4;
    tick(dut, tfp);          // mult_reg = 16, acc_reg = 0 + 9 = 9
    dut->a = 0; dut->b = 0;
    tick(dut, tfp);          // mult_reg = 0, acc_reg = 9 + 16 = 25
    tick(dut, tfp);          // acc_reg = 25 + 0 = 25

    // Sequence 2 result: 3*3 + 4*4 = 9 + 16 = 25
    CHECK(dut->result == 25, "seq2: expected 25, got %d", dut->result);
}

// Test 5: max-value inputs, verify signed multiplication and accumulator width
void test_boundary(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: boundary values\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // Max positive * max positive: 32767 * 32767 = 1,073,676,289
    dut->a = 0x7FFF;
    dut->b = 0x7FFF;
    tick(dut, tfp);    // Stage 1: mult_reg = 1,073,676,289. Stage 2: clear_acc=1, loads old mult_reg (0) -> acc_reg = 0
    dut->clear_acc = 0;

    // Max negative * max positive: -32768 * 32767 = -1,073,709,056
    dut->a = 0x8000;
    dut->b = 0x7FFF;
    tick(dut, tfp);    // acc_reg = 0 + 1,073,676,289 = 1,073,676,289

    dut->a = 0; dut->b = 0;
    tick(dut, tfp);    // acc_reg = 1,073,676,289 + (-1,073,709,056) = -32,767
    tick(dut, tfp);    // drain

    // 32767*32767 + (-32768)*32767 = 32767*(32767 - 32768) = 32767*(-1) = -32767
    int32_t expected = -32767;
    int32_t got = (int32_t)dut->result;
    CHECK(got == expected, "boundary: expected %d, got %d", expected, got);
}

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

// Test 7: signed accumulation with negative products
void test_negative_accumulation(Vmac_unit* dut, VerilatedVcdC* tfp) {
    printf("Test: negative accumulation\n");
    reset(dut, tfp);

    dut->enable = 1;
    dut->clear_acc = 1;

    // (-3) * 4 = -12
    dut->a = (uint16_t)(int16_t)-3;   // 0xFFFD
    dut->b = 4;
    tick(dut, tfp);          // mult_reg = -12, acc_reg = 0 (clear loaded old mult_reg=0)
    dut->clear_acc = 0;

    // 5 * (-6) = -30
    dut->a = 5;
    dut->b = (uint16_t)(int16_t)-6;   // 0xFFFA
    tick(dut, tfp);          // mult_reg = -30, acc_reg = 0 + (-12) = -12

    // (-7) * (-8) = 56
    dut->a = (uint16_t)(int16_t)-7;   // 0xFFF9
    dut->b = (uint16_t)(int16_t)-8;   // 0xFFF8
    tick(dut, tfp);          // mult_reg = 56, acc_reg = -12 + (-30) = -42

    dut->a = 0; dut->b = 0;
    tick(dut, tfp);          // mult_reg = 0, acc_reg = -42 + 56 = 14
    tick(dut, tfp);          // drain: acc_reg = 14 + 0 = 14

    // Expected: (-3)*4 + 5*(-6) + (-7)*(-8) = -12 + (-30) + 56 = 14
    int32_t expected = 14;
    int32_t got = (int32_t)dut->result;
    CHECK(got == expected, "negative accum: expected %d, got %d", expected, got);
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

    test_passthrough_timing(dut, tfp);
    test_basic_accumulation(dut, tfp);
    test_reset(dut, tfp);
    test_clear_midstream(dut, tfp);
    test_boundary(dut, tfp);
    test_enable_stall(dut, tfp);
    test_negative_accumulation(dut, tfp);

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
