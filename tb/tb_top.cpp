#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Vtop.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

// ── Array and data parameters ───────────────────────────────────────────
static const int N   = 4;   // ROWS = COLS
static const int DW  = 16;  // DATA_WIDTH
static const int AW  = 32;  // ACC_WIDTH
static const int A_VEC_W = N * DW;  // bits per SP_A entry
static const int B_VEC_W = N * DW;  // bits per SP_B entry
static const int C_VEC_W = N * AW;  // bits per SP_C entry

static vluint64_t sim_time = 0;
static int test_failures = 0;

#define CHECK(cond, fmt, ...) \
    do { if (!(cond)) { printf("FAIL [%lu]: " fmt "\n", sim_time, ##__VA_ARGS__); test_failures++; } } while(0)

// ── Clock / reset ───────────────────────────────────────────────────────

void tick(Vtop* dut, VerilatedVcdC* tfp) {
    dut->clk = 0;
    dut->eval();
    tfp->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    tfp->dump(sim_time++);
}

void reset(Vtop* dut, VerilatedVcdC* tfp) {
    dut->rst_n = 0;
    dut->start = 0;
    dut->ext_sel = 0;
    dut->ext_addr = 0;
    dut->ext_we = 0;
    for (int i = 0; i < 4; i++) dut->ext_wdata[i] = 0;
    dut->dim_m = 0;
    dut->dim_k = 0;
    dut->dim_n = 0;
    tick(dut, tfp);
    tick(dut, tfp);
    dut->rst_n = 1;
    tick(dut, tfp);
}

// ── External port helpers ────────────────────────────────────────────────
// ext_wdata/ext_rdata is COLS*ACC_WIDTH = 128 bits, stored as WData[4] (uint32_t[4])

// Write a 64-bit value to scratchpad (SP_A or SP_B: 64-bit entries)
void write_sp64(Vtop* dut, VerilatedVcdC* tfp, int sel, int addr, uint64_t val) {
    dut->ext_sel  = sel;
    dut->ext_addr = addr;
    dut->ext_we   = 1;
    dut->ext_wdata[0] = (uint32_t)(val);
    dut->ext_wdata[1] = (uint32_t)(val >> 32);
    dut->ext_wdata[2] = 0;
    dut->ext_wdata[3] = 0;
    tick(dut, tfp);
    dut->ext_we = 0;
}

// Write a 128-bit value to scratchpad (SP_C: 128-bit entries)
void write_sp128(Vtop* dut, VerilatedVcdC* tfp, int sel, int addr,
                 uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    dut->ext_sel  = sel;
    dut->ext_addr = addr;
    dut->ext_we   = 1;
    dut->ext_wdata[0] = w0;
    dut->ext_wdata[1] = w1;
    dut->ext_wdata[2] = w2;
    dut->ext_wdata[3] = w3;
    tick(dut, tfp);
    dut->ext_we = 0;
}

// Read from scratchpad (1-cycle latency: set addr, tick, then read rdata)
void read_sp_start(Vtop* dut, VerilatedVcdC* tfp, int sel, int addr) {
    dut->ext_sel  = sel;
    dut->ext_addr = addr;
    dut->ext_we   = 0;
    tick(dut, tfp);
}

uint64_t read_sp64(Vtop* dut) {
    return (uint64_t)dut->ext_rdata[0] | ((uint64_t)dut->ext_rdata[1] << 32);
}

// ── Pack/unpack helpers ─────────────────────────────────────────────────

// Pack N int16 values into a 64-bit vector (for SP_A / SP_B entries)
uint64_t pack_vec16(int16_t vals[], int count) {
    uint64_t flat = 0;
    for (int i = 0; i < count; i++)
        flat |= ((uint64_t)(uint16_t)vals[i]) << (i * DW);
    return flat;
}

// ── Matrix packing helpers ───────────────────────────────────────────────
// SP_A stores rows of A tiles. Entry t = row t packed as ROWS int16 values.
// Array feeds a_in[k] = A[t][k] at cycle t (matching tb_systolic_array pattern).
// For tile (mt, kt) of matrix A[M×K]:
//   SP_A[base + t] = { A[mt*N + t][kt*N + N-1], ..., A[mt*N + t][kt*N] }
// Base address = (mt * K_tiles + kt) * N

void pack_a_tiles(Vtop* dut, VerilatedVcdC* tfp,
                  int16_t* A, int M, int K) {
    int K_tiles = K / N;
    for (int mt = 0; mt < M / N; mt++) {
        for (int kt = 0; kt < K_tiles; kt++) {
            int base = (mt * K_tiles + kt) * N;
            for (int t = 0; t < N; t++) {
                // Pack row t of this A tile
                int16_t row[N];
                for (int c = 0; c < N; c++)
                    row[c] = A[(mt * N + t) * K + (kt * N + c)];
                write_sp64(dut, tfp, 0, base + t, pack_vec16(row, N));
            }
        }
    }
}

// SP_B stores rows of B tiles. Entry t = row t packed as COLS int16 values.
// For tile (kt, nt) of matrix B[K×N]:
//   SP_B[base + t] = { B[kt*N + t][nt*N + N-1], ..., B[kt*N + t][nt*N] }
// Base address = (kt * N_tiles + nt) * N

void pack_b_tiles(Vtop* dut, VerilatedVcdC* tfp,
                  int16_t* B, int K, int BN) {
    int N_tiles = BN / N;
    for (int kt = 0; kt < K / N; kt++) {
        for (int nt = 0; nt < N_tiles; nt++) {
            int base = (kt * N_tiles + nt) * N;
            for (int t = 0; t < N; t++) {
                int16_t row[N];
                for (int c = 0; c < N; c++)
                    row[c] = B[(kt * N + t) * BN + (nt * N + c)];
                write_sp64(dut, tfp, 1, base + t, pack_vec16(row, N));
            }
        }
    }
}

// Read C tiles from SP_C into row-major matrix.
// SP_C entry t of tile (mt, nt) = row t of C tile, packed as COLS int32 values.
// Base address = (mt * N_tiles + nt) * N

void unpack_c(Vtop* dut, VerilatedVcdC* tfp,
              int32_t* C, int M, int CN) {
    int N_tiles = CN / N;
    for (int mt = 0; mt < M / N; mt++) {
        for (int nt = 0; nt < N_tiles; nt++) {
            int base = (mt * N_tiles + nt) * N;
            for (int t = 0; t < N; t++) {
                read_sp_start(dut, tfp, 2, base + t);
                // ext_rdata is 128 bits = 4 x uint32_t = 4 x ACC_WIDTH values
                for (int c = 0; c < N; c++) {
                    C[(mt * N + t) * CN + (nt * N + c)] = (int32_t)dut->ext_rdata[c];
                }
            }
        }
    }
}

// Reference matmul: C = A * B (int32 accumulation)
void ref_matmul(int16_t* A, int16_t* B, int32_t* C, int M, int K, int BN) {
    for (int i = 0; i < M; i++)
        for (int j = 0; j < BN; j++) {
            int32_t sum = 0;
            for (int k = 0; k < K; k++)
                sum += (int32_t)A[i * K + k] * (int32_t)B[k * BN + j];
            C[i * BN + j] = sum;
        }
}

// Run computation: set dimensions, assert start, poll done.
// Returns the number of cycles from start to done.
int run_compute(Vtop* dut, VerilatedVcdC* tfp, int M, int K, int BN) {
    dut->dim_m = M;
    dut->dim_k = K;
    dut->dim_n = BN;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // Poll for done (with timeout)
    for (int i = 0; i < 10000; i++) {
        tick(dut, tfp);
        if (dut->done) {
            tick(dut, tfp);  // let ctrl_running deassert
            return i + 1;    // cycles from start deassert to done
        }
    }
    printf("TIMEOUT: controller did not assert done\n");
    test_failures++;
    return -1;
}

void check_matrix(int32_t* result, int32_t* expected, int M, int CN, const char* name) {
    for (int i = 0; i < M; i++)
        for (int j = 0; j < CN; j++)
            CHECK(result[i * CN + j] == expected[i * CN + j],
                  "%s: C[%d][%d] = %d, expected %d",
                  name, i, j, result[i * CN + j], expected[i * CN + j]);
}

// ── Test: external port read/write ──────────────────────────────────────

void test_ext_port(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: external port read/write\n");
    reset(dut, tfp);

    // Write to SP_A (sel=0), addr 0 and 1
    write_sp64(dut, tfp, 0, 0, 0xDEADBEEF12345678ULL);
    write_sp64(dut, tfp, 0, 1, 0xCAFEBABE00000001ULL);

    // Write to SP_B (sel=1), addr 0
    write_sp64(dut, tfp, 1, 0, 0x1111222233334444ULL);

    // Write to SP_C (sel=2), addr 0
    write_sp128(dut, tfp, 2, 0, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);

    // Read back SP_A addr 0 (1-cycle latency)
    read_sp_start(dut, tfp, 0, 0);
    uint64_t a0 = read_sp64(dut);
    CHECK(a0 == 0xDEADBEEF12345678ULL,
          "SP_A[0]: expected 0xDEADBEEF12345678, got 0x%016lx", a0);

    // Read back SP_A addr 1
    read_sp_start(dut, tfp, 0, 1);
    uint64_t a1 = read_sp64(dut);
    CHECK(a1 == 0xCAFEBABE00000001ULL,
          "SP_A[1]: expected 0xCAFEBABE00000001, got 0x%016lx", a1);

    // Read back SP_B addr 0
    read_sp_start(dut, tfp, 1, 0);
    uint64_t b0 = read_sp64(dut);
    CHECK(b0 == 0x1111222233334444ULL,
          "SP_B[0]: expected 0x1111222233334444, got 0x%016lx", b0);

    // Read back SP_C addr 0
    read_sp_start(dut, tfp, 2, 0);
    CHECK(dut->ext_rdata[0] == 0xAAAAAAAA, "SP_C[0].w0 mismatch");
    CHECK(dut->ext_rdata[1] == 0xBBBBBBBB, "SP_C[0].w1 mismatch");
    CHECK(dut->ext_rdata[2] == 0xCCCCCCCC, "SP_C[0].w2 mismatch");
    CHECK(dut->ext_rdata[3] == 0xDDDDDDDD, "SP_C[0].w3 mismatch");
}

// ── Test: single tile 4×4 ────────────────────────────────────────────────

void test_single_tile(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: single tile 4x4\n");
    reset(dut, tfp);

    int16_t A[N * N] = {
         1,  2,  3,  4,
         5,  6,  7,  8,
         9, 10, 11, 12,
        13, 14, 15, 16
    };
    int16_t B[N * N] = {
        1, 0, 0, 0,
        0, 2, 0, 0,
        0, 0, 3, 0,
        0, 0, 0, 4
    };
    int32_t expected[N * N];
    int32_t result[N * N] = {};

    ref_matmul(A, B, expected, N, N, N);

    pack_a_tiles(dut, tfp, A, N, N);
    pack_b_tiles(dut, tfp, B, N, N);
    int cycles = run_compute(dut, tfp, N, N, N);
    unpack_c(dut, tfp, result, N, N);

    check_matrix(result, expected, N, N, "single_tile");
    printf("  single_tile: %d cycles\n", cycles);
}

// ── Test: K-tiling (4×8 × 8×4) ─────────────────────────────────────────

void test_k_tiling(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: K-tiling 4x8 * 8x4\n");
    reset(dut, tfp);

    const int M = 4, K = 8, BN = 4;
    int16_t A[M * K], B[K * BN];
    int32_t expected[M * BN], result[M * BN] = {};

    for (int i = 0; i < M * K; i++) A[i] = (int16_t)(i + 1);
    for (int i = 0; i < K * BN; i++) B[i] = (int16_t)(i + 1);

    ref_matmul(A, B, expected, M, K, BN);
    pack_a_tiles(dut, tfp, A, M, K);
    pack_b_tiles(dut, tfp, B, K, BN);
    int cycles = run_compute(dut, tfp, M, K, BN);
    unpack_c(dut, tfp, result, M, BN);
    check_matrix(result, expected, M, BN, "k_tiling");
    printf("  k_tiling (4x8 * 8x4): %d cycles\n", cycles);
}

// ── Test: full tiling (8×8) ─────────────────────────────────────────────

void test_full_tiling(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: full tiling 8x8\n");
    reset(dut, tfp);

    const int M = 8, K = 8, BN = 8;
    int16_t A[M * K], B[K * BN];
    int32_t expected[M * BN], result[M * BN] = {};

    // Fill with small random-ish values
    for (int i = 0; i < M * K; i++) A[i] = (int16_t)((i * 7 + 3) % 31 - 15);
    for (int i = 0; i < K * BN; i++) B[i] = (int16_t)((i * 13 + 5) % 29 - 14);

    ref_matmul(A, B, expected, M, K, BN);
    pack_a_tiles(dut, tfp, A, M, K);
    pack_b_tiles(dut, tfp, B, K, BN);
    int cycles = run_compute(dut, tfp, M, K, BN);
    unpack_c(dut, tfp, result, M, BN);
    check_matrix(result, expected, M, BN, "full_tiling");
    printf("  full_tiling (8x8): %d cycles\n", cycles);
}

// ── Test: reset mid-computation ─────────────────────────────────────────

void test_reset(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: reset mid-computation\n");

    // Start a computation
    reset(dut, tfp);

    int16_t A[N * N] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int16_t B[N * N] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    pack_a_tiles(dut, tfp, A, N, N);
    pack_b_tiles(dut, tfp, B, N, N);

    dut->dim_m = N;
    dut->dim_k = N;
    dut->dim_n = N;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // Let it run a few cycles (mid-compute)
    for (int i = 0; i < 5; i++) tick(dut, tfp);

    // Assert reset
    dut->rst_n = 0;
    tick(dut, tfp);
    tick(dut, tfp);
    dut->rst_n = 1;
    tick(dut, tfp);

    // Controller should be back in IDLE, not done
    CHECK(!dut->done, "reset: done should be 0 after reset");

    // Should be able to run a fresh computation
    int32_t expected[N * N], result[N * N] = {};
    ref_matmul(A, B, expected, N, N, N);

    pack_a_tiles(dut, tfp, A, N, N);
    pack_b_tiles(dut, tfp, B, N, N);
    run_compute(dut, tfp, N, N, N);
    unpack_c(dut, tfp, result, N, N);
    check_matrix(result, expected, N, N, "reset_recovery");
}

// ── Main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vtop* dut = new Vtop;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    if (system("mkdir -p waves") != 0)
        fprintf(stderr, "Warning: could not create waves/ directory\n");
    tfp->open("waves/top.vcd");

    printf("=== tb_top: systolic accelerator integration tests ===\n");

    test_ext_port(dut, tfp);
    test_single_tile(dut, tfp);
    test_k_tiling(dut, tfp);
    test_full_tiling(dut, tfp);
    test_reset(dut, tfp);

    tfp->close();
    delete tfp;
    delete dut;

    if (test_failures == 0)
        printf("\nAll tests PASSED\n");
    else
        printf("\n%d test(s) FAILED\n", test_failures);

    return test_failures ? 1 : 0;
}
