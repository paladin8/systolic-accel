#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Vsystolic_array.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

static const int N = 4;
static const int DW = 16;  // DATA_WIDTH
static const int AW = 32;  // ACC_WIDTH
static vluint64_t sim_time = 0;
static int test_failures = 0;

// Pipeline depth: set via -CFLAGS "-DPIPELINE_DEPTH=N", defaults to 2
#ifndef PIPELINE_DEPTH
#define PIPELINE_DEPTH 2
#endif

// Base offset for drain timing: C[m][j] valid at compute tick base+m+j
// Derivation: activation reaches MAC[k][j] at cycle m+k+j. With D-cycle MAC,
// psum propagates 1 cycle per row (limited by activation stagger). The column
// of N MACs adds N-1 cycles after the first MAC's output. First MAC output
// at cycle j + PIPELINE_DEPTH. So C[m][j] at cycle m + j + N + PIPELINE_DEPTH - 2.
static const int DRAIN_BASE = N + PIPELINE_DEPTH - 2;

#define CHECK(cond, fmt, ...) \
    do { if (!(cond)) { printf("FAIL [%lu]: " fmt "\n", sim_time, ##__VA_ARGS__); test_failures++; } } while(0)

// Pack N int16 values into a flat bit vector
uint64_t pack_row(int16_t vals[N]) {
    uint64_t flat = 0;
    for (int i = 0; i < N; i++)
        flat |= ((uint64_t)(uint16_t)vals[i]) << (i * DW);
    return flat;
}

// Extract int32 result from drain_out at position j
int32_t extract_drain(Vsystolic_array* dut, int j) {
    // drain_out is COLS*ACC_WIDTH = 128 bits, stored as WData array of uint32_t
    return (int32_t)dut->drain_out[j];
}

void tick(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    dut->clk = 0;
    dut->eval();
    tfp->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    tfp->dump(sim_time++);
}

void reset(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    dut->rst_n = 0;
    dut->enable = 0;
    dut->load_weight = 0;
    dut->a_in = 0;
    dut->b_in = 0;
    tick(dut, tfp);
    tick(dut, tfp);
    dut->rst_n = 1;
    tick(dut, tfp);
}

// Load weights B into the array (N cycles, reverse row order)
void load_weights(Vsystolic_array* dut, VerilatedVcdC* tfp, int16_t B[N][N]) {
    dut->enable = 1;
    dut->load_weight = 1;
    dut->a_in = 0;

    for (int t = 0; t < N; t++) {
        int row = N - 1 - t;  // reverse order
        dut->b_in = pack_row(B[row]);
        tick(dut, tfp);
    }

    dut->load_weight = 0;
    dut->b_in = 0;
}

// Feed activations and capture results from drain_out
// C[m][j] valid at drain_out after compute tick DRAIN_BASE+m+j
void feed_and_capture(Vsystolic_array* dut, VerilatedVcdC* tfp,
                      int16_t A[N][N], int32_t result[N][N]) {
    int total_ticks = DRAIN_BASE + 2 * (N - 1) + 2;  // last capture + safety

    for (int t = 0; t < total_ticks; t++) {
        if (t < N)
            dut->a_in = pack_row(A[t]);
        else
            dut->a_in = 0;

        tick(dut, tfp);

        // Capture results: C[m][j] valid after tick DRAIN_BASE+m+j
        for (int m = 0; m < N; m++) {
            for (int j = 0; j < N; j++) {
                if (t == DRAIN_BASE + m + j) {
                    result[m][j] = extract_drain(dut, j);
                }
            }
        }
    }
}

void check_results(int32_t result[N][N], int32_t expected[N][N], const char* test_name) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            CHECK(result[i][j] == expected[i][j],
                  "%s: result[%d][%d] = %d, expected %d",
                  test_name, i, j, result[i][j], expected[i][j]);
        }
    }
}

// Test 1: A = I, B = known matrix. C should equal B.
void test_identity(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: identity multiply\n");
    reset(dut, tfp);

    int16_t A[N][N] = {};
    int16_t B[N][N] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16}
    };
    int32_t expected[N][N] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16}
    };
    int32_t result[N][N] = {};

    for (int i = 0; i < N; i++) A[i][i] = 1;

    load_weights(dut, tfp, B);
    feed_and_capture(dut, tfp, A, result);
    check_results(result, expected, "identity");
}

// Test 2: A[1][2]=5, B[2][3]=7, all else 0. Only result[1][3]=35.
void test_single_element(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: single nonzero element\n");
    reset(dut, tfp);

    int16_t A[N][N] = {};
    int16_t B[N][N] = {};
    A[1][2] = 5;
    B[2][3] = 7;

    int32_t expected[N][N] = {};
    expected[1][3] = 35;

    int32_t result[N][N] = {};

    load_weights(dut, tfp, B);
    feed_and_capture(dut, tfp, A, result);
    check_results(result, expected, "single_element");
}

// Test 3: A = arbitrary, B = 0. All results should be 0.
void test_zero_matrix(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: zero matrix\n");
    reset(dut, tfp);

    int16_t A[N][N] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16}
    };
    int16_t B[N][N] = {};
    int32_t expected[N][N] = {};
    int32_t result[N][N] = {};

    load_weights(dut, tfp, B);
    feed_and_capture(dut, tfp, A, result);
    check_results(result, expected, "zero_matrix");
}

// Test 4: A[i][j] = i*4+j+1, B = I. Result should equal A.
void test_counting(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: counting matrix\n");
    reset(dut, tfp);

    int16_t A[N][N];
    int16_t B[N][N] = {};
    int32_t expected[N][N];
    int32_t result[N][N] = {};

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            A[i][j] = i * N + j + 1;

    for (int i = 0; i < N; i++) B[i][i] = 1;

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            expected[i][j] = A[i][j];

    load_weights(dut, tfp, B);
    feed_and_capture(dut, tfp, A, result);
    check_results(result, expected, "counting");
}

// Test 5: mixed positive/negative values
void test_negative(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: negative values\n");
    reset(dut, tfp);

    int16_t A[N][N] = {
        { 1, -2,  3, -4},
        {-5,  6, -7,  8},
        { 9,-10, 11,-12},
        {-1,  2, -3,  4}
    };
    int16_t B[N][N] = {
        { 2,  0,  0,  0},
        { 0, -3,  0,  0},
        { 0,  0,  4,  0},
        { 0,  0,  0, -5}
    };
    int32_t expected[N][N] = {
        { 2,   6, 12,  20},
        {-10,-18,-28, -40},
        { 18, 30, 44,  60},
        { -2, -6,-12, -20}
    };
    int32_t result[N][N] = {};

    load_weights(dut, tfp, B);
    feed_and_capture(dut, tfp, A, result);
    check_results(result, expected, "negative");
}

// Test 6: Load weights once, compute two different matmuls back-to-back
void test_weight_reuse(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: weight reuse\n");
    reset(dut, tfp);

    int16_t B[N][N] = {
        {1, 0, 0, 0},
        {0, 2, 0, 0},
        {0, 0, 3, 0},
        {0, 0, 0, 4}
    };

    int16_t A1[N][N] = {
        {1, 1, 1, 1},
        {2, 2, 2, 2},
        {3, 3, 3, 3},
        {4, 4, 4, 4}
    };
    int32_t expected1[N][N] = {
        {1, 2, 3, 4},
        {2, 4, 6, 8},
        {3, 6, 9, 12},
        {4, 8, 12, 16}
    };

    int16_t A2[N][N] = {
        {10, 20, 30, 40},
        {-1, -2, -3, -4},
        { 0,  0,  0,  0},
        { 5,  5,  5,  5}
    };
    int32_t expected2[N][N] = {
        {10, 40,  90, 160},
        {-1, -4,  -9, -16},
        { 0,  0,   0,   0},
        { 5, 10,  15,  20}
    };

    int32_t result[N][N] = {};

    load_weights(dut, tfp, B);

    feed_and_capture(dut, tfp, A1, result);
    check_results(result, expected1, "weight_reuse_1");

    memset(result, 0, sizeof(result));
    feed_and_capture(dut, tfp, A2, result);
    check_results(result, expected2, "weight_reuse_2");
}

// Test 7: Verify pipeline timing
// C[0][0] at cycle DRAIN_BASE, C[N-1][N-1] at cycle DRAIN_BASE+2(N-1)
void test_timing(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: timing verification\n");
    reset(dut, tfp);

    int16_t A[N][N], B[N][N];
    int32_t expected[N][N];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = i * N + j + 1;
            B[i][j] = (i == j) ? 1 : 0;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            expected[i][j] = A[i][j];

    load_weights(dut, tfp, B);

    int first_tick = DRAIN_BASE;                     // C[0][0]
    int penult_tick = DRAIN_BASE + 2 * (N - 1) - 1; // C[N-2][N-1]
    int last_tick = DRAIN_BASE + 2 * (N - 1);        // C[N-1][N-1]
    int total_ticks = last_tick + 2;

    for (int t = 0; t < total_ticks; t++) {
        if (t < N)
            dut->a_in = pack_row(A[t]);
        else
            dut->a_in = 0;

        tick(dut, tfp);

        if (t == first_tick) {
            int32_t val = extract_drain(dut, 0);
            CHECK(val == expected[0][0],
                  "timing: C[0][0] at tick %d: expected %d, got %d",
                  t, expected[0][0], val);
        }

        if (t == penult_tick) {
            int32_t val = extract_drain(dut, N - 1);
            CHECK(val == expected[N-2][N-1],
                  "timing: drain_out[%d] at tick %d: expected C[%d][%d]=%d, got %d",
                  N-1, t, N-2, N-1, expected[N-2][N-1], val);
        }

        if (t == last_tick) {
            int32_t val = extract_drain(dut, N - 1);
            CHECK(val == expected[N-1][N-1],
                  "timing: C[%d][%d] at tick %d: expected %d, got %d",
                  N-1, N-1, t, expected[N-1][N-1], val);
        }
    }
}

// Test 8: Load test vectors from Python-generated file
void test_random(Vsystolic_array* dut, VerilatedVcdC* tfp) {
    printf("Test: random (file-loaded)\n");

    FILE* f = fopen("tb/test_vectors.txt", "r");
    if (!f) {
        printf("SKIP: could not open tb/test_vectors.txt\n");
        return;
    }

    int n;
    if (fscanf(f, "%d", &n) != 1 || n != N) {
        printf("SKIP: test_vectors.txt has n=%d, expected %d\n", n, N);
        fclose(f);
        return;
    }

    int16_t A[N][N];
    int16_t B[N][N];
    int32_t expected[N][N];
    int32_t result[N][N] = {};

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int v; fscanf(f, "%d", &v);
            A[i][j] = (int16_t)v;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int v; fscanf(f, "%d", &v);
            B[i][j] = (int16_t)v;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int v; fscanf(f, "%d", &v);
            expected[i][j] = (int32_t)v;
        }
    fclose(f);

    reset(dut, tfp);
    load_weights(dut, tfp, B);
    feed_and_capture(dut, tfp, A, result);
    check_results(result, expected, "random");
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vsystolic_array* dut = new Vsystolic_array;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    if (system("mkdir -p waves") != 0) {
        fprintf(stderr, "Warning: could not create waves/ directory\n");
    }
    tfp->open("waves/systolic_array.vcd");

    printf("Pipeline depth: %d, drain base: %d\n", PIPELINE_DEPTH, DRAIN_BASE);
    test_identity(dut, tfp);
    test_single_element(dut, tfp);
    test_zero_matrix(dut, tfp);
    test_counting(dut, tfp);
    test_negative(dut, tfp);
    test_weight_reuse(dut, tfp);
    test_timing(dut, tfp);
    test_random(dut, tfp);

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
