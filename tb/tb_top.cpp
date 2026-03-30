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
    dut->bank_sel = 0;
    dut->ext_bank = 0;
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

// Read C from the bank that was just computed on.
// With double-buffered SP_C, ext_bank must match the compute bank.
void unpack_c_bank(Vtop* dut, VerilatedVcdC* tfp,
                   int32_t* C, int M, int CN, int bank) {
    dut->ext_bank = bank;
    unpack_c(dut, tfp, C, M, CN);
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

// ── Test: bank select ───────────────────────────────────────────────────

void test_bank_select(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: bank select\n");
    reset(dut, tfp);

    int16_t A[N * N] = {
         1,  2,  3,  4,
         5,  6,  7,  8,
         9, 10, 11, 12,
        13, 14, 15, 16
    };
    int16_t B[N * N] = {
        2, 0, 0, 0,
        0, 3, 0, 0,
        0, 0, 4, 0,
        0, 0, 0, 5
    };
    int32_t expected[N * N], result[N * N] = {};
    ref_matmul(A, B, expected, N, N, N);

    // Load A/B into bank 0
    dut->ext_bank = 0;
    pack_a_tiles(dut, tfp, A, N, N);
    pack_b_tiles(dut, tfp, B, N, N);

    // Load same A/B into bank 1
    dut->ext_bank = 1;
    pack_a_tiles(dut, tfp, A, N, N);
    pack_b_tiles(dut, tfp, B, N, N);

    // Compute from bank 0
    dut->bank_sel = 0;
    int cyc0 = run_compute(dut, tfp, N, N, N);
    unpack_c_bank(dut, tfp, result, N, N, 0);
    check_matrix(result, expected, N, N, "bank_select_bank0");

    // Reset to get controller back to S_IDLE (scratchpad data persists)
    reset(dut, tfp);

    // Compute from bank 1 (SP_C clearing unnecessary — kt=0 does direct write)
    dut->bank_sel = 1;
    memset(result, 0, sizeof(result));
    int cyc1 = run_compute(dut, tfp, N, N, N);
    unpack_c_bank(dut, tfp, result, N, N, 1);
    check_matrix(result, expected, N, N, "bank_select_bank1");

    CHECK(cyc0 == cyc1, "bank_select: cycle mismatch bank0=%d bank1=%d", cyc0, cyc1);

    // Verify bank_sel latching: changing bank_sel mid-computation has no effect.
    // Load different data into bank 0 vs bank 1.
    reset(dut, tfp);
    int16_t A2[N * N], B2[N * N];
    for (int i = 0; i < N * N; i++) {
        A2[i] = (int16_t)(i + 100);
        B2[i] = (int16_t)(i + 200);
    }
    dut->ext_bank = 1;
    pack_a_tiles(dut, tfp, A2, N, N);
    pack_b_tiles(dut, tfp, B2, N, N);

    // Start on bank 0, then flip bank_sel to 1 mid-computation
    dut->bank_sel = 0;
    dut->dim_m = N;
    dut->dim_k = N;
    dut->dim_n = N;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // Flip bank_sel after a few cycles
    for (int i = 0; i < 5; i++) tick(dut, tfp);
    dut->bank_sel = 1;  // should have no effect — latched on start

    for (int i = 0; i < 10000; i++) {
        tick(dut, tfp);
        if (dut->done) { tick(dut, tfp); break; }
    }
    CHECK(dut->done, "bank_select_latch: computation did not finish");

    // Result should match bank 0's data (A/B from bank 0), not bank 1's (A2/B2)
    // Read from C bank 0 (the latched active_bank)
    memset(result, 0, sizeof(result));
    unpack_c_bank(dut, tfp, result, N, N, 0);
    check_matrix(result, expected, N, N, "bank_select_latch");

    printf("  bank_select: bank0=%d cycles, bank1=%d cycles, latch=OK\n", cyc0, cyc1);
}

// ── Test: double buffer ─────────────────────────────────────────────────
// Full ping-pong workflow: load one bank while computing from the other.

// Helper: load A/B into a specific bank, run compute, verify against ref.
// Does NOT reset — caller manages controller state.
void load_bank(Vtop* dut, VerilatedVcdC* tfp, int bank,
               int16_t* A, int M, int K, int16_t* B, int BN) {
    dut->ext_bank = bank;
    pack_a_tiles(dut, tfp, A, M, K);
    pack_b_tiles(dut, tfp, B, K, BN);
}

// Helper: load A/B into a bank while controller is running.
// Issues writes interleaved with ticks, checking done each cycle.
// Returns number of extra ticks after loading completes before done.
void load_bank_during_compute(Vtop* dut, VerilatedVcdC* tfp, int bank,
                              int16_t* A, int M, int K, int16_t* B, int BN) {
    dut->ext_bank = bank;
    // pack_a_tiles/pack_b_tiles use write_sp64 which ticks the clock,
    // so the controller advances while we load.
    pack_a_tiles(dut, tfp, A, M, K);
    pack_b_tiles(dut, tfp, B, K, BN);
}

// Helper: read back a single SP_A entry from a specific bank and verify
uint64_t read_bank_sp64(Vtop* dut, VerilatedVcdC* tfp, int bank, int sel, int addr) {
    dut->ext_bank = bank;
    read_sp_start(dut, tfp, sel, addr);
    return read_sp64(dut);
}

void test_double_buffer(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: double buffer (4x4)\n");
    reset(dut, tfp);

    // Three distinct matrix pairs with non-overlapping value ranges
    int16_t A0[N * N], B0[N * N];
    int16_t A1[N * N], B1[N * N];
    int16_t A2[N * N], B2[N * N];

    for (int i = 0; i < N * N; i++) {
        A0[i] = (int16_t)(i + 1);          // 1..16
        B0[i] = (int16_t)(i + 17);         // 17..32
        A1[i] = (int16_t)(i + 50);         // 50..65
        B1[i] = (int16_t)(i + 70);         // 70..85
        A2[i] = (int16_t)(i + 100);        // 100..115
        B2[i] = (int16_t)(i + 200);        // 200..215
    }

    int32_t ref0[N * N], ref2[N * N];
    ref_matmul(A0, B0, ref0, N, N, N);
    ref_matmul(A2, B2, ref2, N, N, N);

    int32_t result[N * N] = {};

    // Step 1-2: Load A0/B0 into bank 0, A1/B1 into bank 1
    load_bank(dut, tfp, 0, A0, N, N, B0, N);
    load_bank(dut, tfp, 1, A1, N, N, B1, N);

    // Step 3: Start computation from bank 0
    dut->bank_sel = 0;
    dut->dim_m = N;
    dut->dim_k = N;
    dut->dim_n = N;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // Step 4: While running, read bank 1 data and verify A1/B1 are intact
    uint64_t a1_row0 = read_bank_sp64(dut, tfp, 1, 0, 0);  // SP_A bank 1
    int16_t a1_check[N];
    for (int c = 0; c < N; c++)
        a1_check[c] = A1[c];  // row 0 of A1
    uint64_t a1_expected = pack_vec16(a1_check, N);
    CHECK(a1_row0 == a1_expected,
          "double_buffer: bank1 A1 readback mismatch during compute: got 0x%016lx, expected 0x%016lx",
          a1_row0, a1_expected);

    uint64_t b1_row0 = read_bank_sp64(dut, tfp, 1, 1, 0);  // SP_B bank 1
    int16_t b1_check[N];
    for (int c = 0; c < N; c++)
        b1_check[c] = B1[c];  // row 0 of B1
    uint64_t b1_expected = pack_vec16(b1_check, N);
    CHECK(b1_row0 == b1_expected,
          "double_buffer: bank1 B1 readback mismatch during compute: got 0x%016lx, expected 0x%016lx",
          b1_row0, b1_expected);

    // Step 5: While running, overwrite bank 1 with A2/B2
    CHECK(!dut->done, "double_buffer: compute finished before mid-compute write started");
    load_bank_during_compute(dut, tfp, 1, A2, N, N, B2, N);

    // Step 6: Wait for done, counting cycles
    int cyc1 = 0;
    for (int i = 0; i < 10000; i++) {
        tick(dut, tfp);
        cyc1++;
        if (dut->done) {
            tick(dut, tfp);
            break;
        }
    }
    CHECK(dut->done, "double_buffer: first computation did not finish");

    // Step 7: Read SP_C bank 0, verify matches ref_matmul(A0, B0)
    unpack_c_bank(dut, tfp, result, N, N, 0);
    check_matrix(result, ref0, N, N, "double_buffer_round1");

    // Step 8: Reset to get controller back to S_IDLE (S_DONE is a dead-end;
    // changing the FSM is outside M6 scope). Scratchpad data persists across reset.
    // Start computation from bank 1 — data was loaded during step 5.
    // No SP_C clear needed — kt=0 does direct write, overwriting stale data.
    reset(dut, tfp);

    dut->bank_sel = 1;
    dut->dim_m = N;
    dut->dim_k = N;
    dut->dim_n = N;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // Step 9: While running, reload A0/B0 into bank 0
    CHECK(!dut->done, "double_buffer: round2 finished before mid-compute load started");
    load_bank_during_compute(dut, tfp, 0, A0, N, N, B0, N);

    // Step 10: Wait for done
    int cyc2 = 0;
    for (int i = 0; i < 10000; i++) {
        tick(dut, tfp);
        cyc2++;
        if (dut->done) {
            tick(dut, tfp);
            break;
        }
    }
    CHECK(dut->done, "double_buffer: second computation did not finish");

    // Step 11: Verify matches ref_matmul(A2, B2) — NOT A1/B1
    memset(result, 0, sizeof(result));
    unpack_c_bank(dut, tfp, result, N, N, 1);
    check_matrix(result, ref2, N, N, "double_buffer_round2_A2B2");

    // Step 12: Reset, compute from bank 0 (reloaded in step 9)
    reset(dut, tfp);

    dut->bank_sel = 0;
    int cyc3 = run_compute(dut, tfp, N, N, N);

    // Step 14: Verify matches ref_matmul(A0, B0) — full round-trip
    memset(result, 0, sizeof(result));
    unpack_c_bank(dut, tfp, result, N, N, 0);
    check_matrix(result, ref0, N, N, "double_buffer_round3_roundtrip");

    // Cycle count check: round3 is a clean run_compute, should be 20 cycles
    // (same as single-tile baseline — no bank switching overhead).
    // round2's partial count (cyc2) excludes cycles consumed by mid-compute loading.
    CHECK(cyc3 == 20, "double_buffer: round3 expected 20 cycles, got %d", cyc3);
    printf("  double_buffer (4x4): round3=%d cycles (round2 remaining=%d)\n", cyc3, cyc2);
}

// ── Test: double buffer tiled (8x8 on 4x4 array) ───────────────────────

void test_double_buffer_tiled(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: double buffer tiled (8x8)\n");
    reset(dut, tfp);

    const int M = 8, K = 8, BN = 8;

    int16_t A0[M * K], B0[K * BN];
    int16_t A1[M * K], B1[K * BN];

    for (int i = 0; i < M * K; i++) {
        A0[i] = (int16_t)((i * 3 + 1) % 31 - 15);
        A1[i] = (int16_t)((i * 7 + 5) % 37 - 18);
    }
    for (int i = 0; i < K * BN; i++) {
        B0[i] = (int16_t)((i * 5 + 2) % 29 - 14);
        B1[i] = (int16_t)((i * 11 + 3) % 41 - 20);
    }

    int32_t ref0[M * BN], ref1[M * BN];
    ref_matmul(A0, B0, ref0, M, K, BN);
    ref_matmul(A1, B1, ref1, M, K, BN);

    int32_t result[M * BN] = {};

    // Load A0/B0 into bank 0
    load_bank(dut, tfp, 0, A0, M, K, B0, BN);

    // Compute from bank 0
    dut->bank_sel = 0;
    dut->dim_m = M;
    dut->dim_k = K;
    dut->dim_n = BN;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // While running, load A1/B1 into bank 1
    load_bank_during_compute(dut, tfp, 1, A1, M, K, B1, BN);

    // Wait for done
    for (int i = 0; i < 10000; i++) {
        tick(dut, tfp);
        if (dut->done) {
            tick(dut, tfp);
            break;
        }
    }
    CHECK(dut->done, "double_buffer_tiled: first computation did not finish");

    // Verify first result (computed on bank 0)
    unpack_c_bank(dut, tfp, result, M, BN, 0);
    check_matrix(result, ref0, M, BN, "double_buffer_tiled_round1");

    // Reset, compute from bank 1
    reset(dut, tfp);
    dut->bank_sel = 1;
    int cyc2 = run_compute(dut, tfp, M, K, BN);

    // Verify second result (computed on bank 1)
    memset(result, 0, sizeof(result));
    unpack_c_bank(dut, tfp, result, M, BN, 1);
    check_matrix(result, ref1, M, BN, "double_buffer_tiled_round2");

    printf("  double_buffer_tiled (8x8): round2=%d cycles\n", cyc2);
}

// ── Test: active bank write protection ──────────────────────────────────

void test_active_bank_protection(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Test: active bank protection\n");
    reset(dut, tfp);

    // Load known data into bank 0
    int16_t A[N * N], B[N * N];
    for (int i = 0; i < N * N; i++) {
        A[i] = (int16_t)(i + 1);
        B[i] = (int16_t)(i + 17);
    }
    dut->ext_bank = 0;
    pack_a_tiles(dut, tfp, A, N, N);
    pack_b_tiles(dut, tfp, B, N, N);

    // Record what we wrote to SP_A bank 0
    uint64_t a_orig[N], b_orig[N];
    for (int t = 0; t < N; t++) {
        int16_t row[N];
        for (int c = 0; c < N; c++) row[c] = A[t * N + c];
        a_orig[t] = pack_vec16(row, N);
    }
    for (int t = 0; t < N; t++) {
        int16_t row[N];
        for (int c = 0; c < N; c++) row[c] = B[t * N + c];
        b_orig[t] = pack_vec16(row, N);
    }

    // Start computation on bank 0
    dut->bank_sel = 0;
    dut->dim_m = N;
    dut->dim_k = N;
    dut->dim_n = N;
    dut->start = 1;
    tick(dut, tfp);
    dut->start = 0;

    // Attempt to write garbage to bank 0 while controller is computing from it
    dut->ext_bank = 0;
    for (int i = 0; i < N; i++) {
        write_sp64(dut, tfp, 0, i, 0xFFFFFFFFFFFFFFFFULL);  // SP_A bank 0
        write_sp64(dut, tfp, 1, i, 0xFFFFFFFFFFFFFFFFULL);  // SP_B bank 0
    }

    // Wait for done
    for (int i = 0; i < 10000; i++) {
        tick(dut, tfp);
        if (dut->done) {
            tick(dut, tfp);
            break;
        }
    }
    CHECK(dut->done, "active_bank_protection: computation did not finish");

    // Verify computation produced correct results (proves controller read original data)
    int32_t expected[N * N], result[N * N] = {};
    ref_matmul(A, B, expected, N, N, N);
    unpack_c_bank(dut, tfp, result, N, N, 0);
    check_matrix(result, expected, N, N, "active_bank_protection_result");

    // Reset, then verify bank 0 data is intact (writes were ignored)
    reset(dut, tfp);
    dut->ext_bank = 0;
    for (int t = 0; t < N; t++) {
        uint64_t a_read = read_bank_sp64(dut, tfp, 0, 0, t);
        CHECK(a_read == a_orig[t],
              "active_bank_protection: SP_A[%d] corrupted: got 0x%016lx, expected 0x%016lx",
              t, a_read, a_orig[t]);
    }
    for (int t = 0; t < N; t++) {
        uint64_t b_read = read_bank_sp64(dut, tfp, 0, 1, t);
        CHECK(b_read == b_orig[t],
              "active_bank_protection: SP_B[%d] corrupted: got 0x%016lx, expected 0x%016lx",
              t, b_read, b_orig[t]);
    }

    printf("  active_bank_protection: PASSED (writes to active bank ignored)\n");
}

// ── Benchmark: double buffer throughput ──────────────────────────────────
// Measures total wall-clock cycles for back-to-back matrix multiplications
// under single-buffered vs double-buffered workflows.

void bench_double_buffer(Vtop* dut, VerilatedVcdC* tfp) {
    printf("Benchmark: double buffer throughput\n");

    const int BATCHES = 4;
    const int M = 8, K = 8, BN = 8;  // tiled: 2x2x2 = 8 tiles per batch

    // Generate distinct matrix pairs per batch
    int16_t A[BATCHES][M * K], B[BATCHES][K * BN];
    int32_t ref[BATCHES][M * BN];
    for (int b = 0; b < BATCHES; b++) {
        for (int i = 0; i < M * K; i++)
            A[b][i] = (int16_t)((i * (b + 3) + b * 17 + 1) % 51 - 25);
        for (int i = 0; i < K * BN; i++)
            B[b][i] = (int16_t)((i * (b + 7) + b * 13 + 2) % 43 - 21);
        ref_matmul(A[b], B[b], ref[b], M, K, BN);
    }

    int32_t result[M * BN];
    vluint64_t t_start, t_end;

    // ── Single-buffered workflow ──────────────────────────────────
    // For each batch: load → compute → readback (all serial)
    reset(dut, tfp);
    t_start = sim_time;

    for (int b = 0; b < BATCHES; b++) {
        // Load
        dut->ext_bank = 0;
        pack_a_tiles(dut, tfp, A[b], M, K);
        pack_b_tiles(dut, tfp, B[b], K, BN);

        // Clear SP_C
        for (int i = 0; i < M * BN / N; i++)
            write_sp128(dut, tfp, 2, i, 0, 0, 0, 0);

        // Compute
        dut->bank_sel = 0;
        run_compute(dut, tfp, M, K, BN);

        // Readback + verify (compute was on bank 0)
        memset(result, 0, sizeof(result));
        unpack_c_bank(dut, tfp, result, M, BN, 0);
        check_matrix(result, ref[b], M, BN, "bench_single_buf");

        // Reset for next batch (controller stuck in S_DONE)
        if (b < BATCHES - 1)
            reset(dut, tfp);
    }

    t_end = sim_time;
    int single_cycles = (int)((t_end - t_start) / 2);  // sim_time increments 2x per tick

    // ── Double-buffered workflow ──────────────────────────────────
    // Overlap: while computing from bank X, load next batch into bank !X
    reset(dut, tfp);
    t_start = sim_time;

    // Load first batch into bank 0
    dut->ext_bank = 0;
    pack_a_tiles(dut, tfp, A[0], M, K);
    pack_b_tiles(dut, tfp, B[0], K, BN);

    for (int b = 0; b < BATCHES; b++) {
        int cur_bank = b & 1;
        int next_bank = 1 - cur_bank;

        // No SP_C clear needed — kt=0 does direct write, overwriting stale data.

        // Start compute on current bank
        dut->bank_sel = cur_bank;
        dut->dim_m = M;
        dut->dim_k = K;
        dut->dim_n = BN;
        dut->start = 1;
        tick(dut, tfp);
        dut->start = 0;

        // While computing: load next A/B into other bank AND read previous
        // results from the other C bank (both overlap with compute).
        if (b < BATCHES - 1) {
            dut->ext_bank = next_bank;
            pack_a_tiles(dut, tfp, A[b + 1], M, K);
            pack_b_tiles(dut, tfp, B[b + 1], K, BN);
        }
        if (b > 0) {
            // Read previous batch's results from the other C bank during compute
            int prev_bank = 1 - cur_bank;
            int32_t prev_result[M * BN];
            memset(prev_result, 0, sizeof(prev_result));
            unpack_c_bank(dut, tfp, prev_result, M, BN, prev_bank);
            check_matrix(prev_result, ref[b - 1], M, BN, "bench_double_buf_overlap");
        }

        // Wait for compute to finish
        for (int i = 0; i < 10000; i++) {
            tick(dut, tfp);
            if (dut->done) {
                tick(dut, tfp);
                break;
            }
        }
        CHECK(dut->done, "bench_double_buf: batch %d did not finish", b);

        // Reset for next batch
        if (b < BATCHES - 1)
            reset(dut, tfp);
    }

    // Read final batch's results
    {
        int final_bank = (BATCHES - 1) & 1;
        memset(result, 0, sizeof(result));
        unpack_c_bank(dut, tfp, result, M, BN, final_bank);
        check_matrix(result, ref[BATCHES - 1], M, BN, "bench_double_buf_final");
    }

    t_end = sim_time;
    int double_cycles = (int)((t_end - t_start) / 2);

    float speedup = (float)single_cycles / (float)double_cycles;
    printf("  single-buffered: %d cycles for %d batches (%d/batch)\n",
           single_cycles, BATCHES, single_cycles / BATCHES);
    printf("  double-buffered: %d cycles for %d batches (%d/batch)\n",
           double_cycles, BATCHES, double_cycles / BATCHES);
    printf("  speedup: %.2fx\n", speedup);
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
    test_bank_select(dut, tfp);
    test_double_buffer(dut, tfp);
    test_double_buffer_tiled(dut, tfp);
    test_active_bank_protection(dut, tfp);
    bench_double_buffer(dut, tfp);

    tfp->close();
    delete tfp;
    delete dut;

    if (test_failures == 0)
        printf("\nAll tests PASSED\n");
    else
        printf("\n%d test(s) FAILED\n", test_failures);

    return test_failures ? 1 : 0;
}
