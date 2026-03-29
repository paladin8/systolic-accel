// Tiled matrix-multiply controller FSM.
// Drives the systolic array through LOAD_WEIGHTS → FEED → DRAIN → WRITEBACK
// phases for each (mt, nt, kt) tile, with read-modify-write accumulation
// for K-dimension tiling. Assumes PIPELINE_DEPTH=2.
//
// Scratchpad read latency: 1 cycle. The controller sets the address one cycle
// before it needs the data. Pre-reads happen at state transitions:
//   - IDLE (start) sets SP_B addr for first LOAD_WEIGHTS cycle
//   - Last LOAD_WEIGHTS cycle sets SP_A addr for first FEED cycle
//   - Last DRAIN cycle sets SP_C addr for first WRITEBACK RMW read
//   - Last WRITEBACK cycle sets SP_B addr for next tile's LOAD_WEIGHTS
module controller #(
    parameter ROWS           = 4,
    parameter COLS           = 4,
    parameter DATA_WIDTH     = 16,
    parameter ACC_WIDTH      = 32,
    parameter PIPELINE_DEPTH = 2,
    parameter SP_A_DEPTH     = 64,
    parameter SP_B_DEPTH     = 64,
    parameter SP_C_DEPTH     = 64
)(
    input  logic                              clk,
    input  logic                              rst_n,

    // Host interface
    input  logic                              start,
    output logic                              done,
    input  logic [15:0]                       dim_m,
    input  logic [15:0]                       dim_k,
    input  logic [15:0]                       dim_n,

    // SP_A read port
    output logic [$clog2(SP_A_DEPTH)-1:0]     sp_a_addr,
    input  logic [ROWS*DATA_WIDTH-1:0]        sp_a_rdata,

    // SP_B read port
    output logic [$clog2(SP_B_DEPTH)-1:0]     sp_b_addr,
    input  logic [COLS*DATA_WIDTH-1:0]        sp_b_rdata,

    // SP_C read/write port
    output logic [$clog2(SP_C_DEPTH)-1:0]     sp_c_addr,
    output logic                              sp_c_we,
    output logic [COLS*ACC_WIDTH-1:0]         sp_c_wdata,
    input  logic [COLS*ACC_WIDTH-1:0]         sp_c_rdata,

    // Array control
    output logic                              arr_enable,
    output logic                              arr_load_weight,
    output logic [ROWS*DATA_WIDTH-1:0]        arr_a_in,
    output logic [COLS*DATA_WIDTH-1:0]        arr_b_in,
    input  logic [COLS*ACC_WIDTH-1:0]         arr_drain_out
);

    // ── FSM states ──────────────────────────────────────────────────────
    typedef enum logic [2:0] {
        S_IDLE         = 3'd0,
        S_LOAD_WEIGHTS = 3'd1,
        S_FEED         = 3'd2,
        S_DRAIN        = 3'd3,
        S_WRITEBACK    = 3'd4,
        S_DONE         = 3'd5
    } state_t;

    state_t state, next_state;

    // ── Tile indices ────────────────────────────────────────────────────
    logic [15:0] mt, nt, kt;
    logic [15:0] mt_max, nt_max, kt_max;

    // ── Phase counter ───────────────────────────────────────────────────
    logic [15:0] phase_cnt;

    // ── Drain timing ────────────────────────────────────────────────────
    // always_ff captures arr_drain_out one cycle after it becomes valid
    // (reads pre-posedge value). First valid capture at phase_cnt=1.
    // Formula: m = phase_cnt - 1 - j.  Total: ROWS + COLS cycles.
    localparam int DRAIN_CYCLES = ROWS + COLS;

    // ── Drain register file ─────────────────────────────────────────────
    // Synthesizes to: ROWS * COLS * ACC_WIDTH flip-flops
    logic [ACC_WIDTH-1:0] drain_regs [0:ROWS-1][0:COLS-1];

    // ── Base addresses (combinational from tile indices) ────────────────
    logic [$clog2(SP_A_DEPTH)-1:0] a_base;
    logic [$clog2(SP_B_DEPTH)-1:0] b_base;
    logic [$clog2(SP_C_DEPTH)-1:0] c_base;

    always_comb begin
        a_base = ($clog2(SP_A_DEPTH))'((mt * kt_max + kt) * 16'(ROWS));
        b_base = ($clog2(SP_B_DEPTH))'((kt * nt_max + nt) * 16'(ROWS));
        c_base = ($clog2(SP_C_DEPTH))'((mt * nt_max + nt) * 16'(ROWS));
    end

    // ── Next-tile indices and pre-read addresses ─────────────────────────
    // Loop order: mt innermost, kt middle, nt outermost (weight-stationary).
    // Weights change only when kt or nt advances (mt wraps).
    logic [15:0] next_kt, next_nt, next_mt;
    logic [$clog2(SP_B_DEPTH)-1:0] next_b_base;
    logic [$clog2(SP_A_DEPTH)-1:0] next_a_base;
    logic next_needs_load;  // weights change on next tile?

    always_comb begin
        if (mt + 1'b1 < mt_max) begin
            // mt advances — same weights
            next_mt = mt + 1'b1;
            next_kt = kt;
            next_nt = nt;
        end else if (kt + 1'b1 < kt_max) begin
            // kt advances — new weights
            next_mt = '0;
            next_kt = kt + 1'b1;
            next_nt = nt;
        end else begin
            // nt advances — new weights
            next_mt = '0;
            next_kt = '0;
            next_nt = nt + 1'b1;
        end
        next_b_base = ($clog2(SP_B_DEPTH))'((next_kt * nt_max + next_nt) * 16'(ROWS));
        next_a_base = ($clog2(SP_A_DEPTH))'((next_mt * kt_max + next_kt) * 16'(ROWS));
        next_needs_load = !(mt + 1'b1 < mt_max);
    end

    // ── More tiles remaining? ───────────────────────────────────────────
    logic more_tiles;
    assign more_tiles = (mt + 1'b1 < mt_max) ||
                        (kt + 1'b1 < kt_max) ||
                        (nt + 1'b1 < nt_max);

    // ── Writeback helpers ───────────────────────────────────────────────
    logic wb_is_rmw;
    assign wb_is_rmw = (kt != '0);

    // RMW writeback: even phase_cnt = write (data ready), odd = read (set addr)
    // Direct writeback: phase_cnt = row index
    logic [15:0] wb_row;
    always_comb begin
        if (wb_is_rmw)
            wb_row = phase_cnt >> 1;  // write on even: row = phase_cnt/2
        else
            wb_row = phase_cnt;
    end

    // WRITEBACK duration
    logic [15:0] wb_last;
    always_comb begin
        if (wb_is_rmw)
            wb_last = 16'(2 * ROWS - 1);  // pre-read from DRAIN saves 1 cycle
        else
            wb_last = 16'(ROWS - 1);
    end

    // ── Tile count computation ──────────────────────────────────────────
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mt_max <= '0;
            nt_max <= '0;
            kt_max <= '0;
        end else if (state == S_IDLE && start) begin
            mt_max <= dim_m / 16'(ROWS);
            nt_max <= dim_n / 16'(COLS);
            kt_max <= dim_k / 16'(ROWS);
        end
    end

    // ── State register ──────────────────────────────────────────────────
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            state <= S_IDLE;
        else
            state <= next_state;
    end

    // ── Phase counter ───────────────────────────────────────────────────
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            phase_cnt <= '0;
        else if (next_state != state)
            phase_cnt <= '0;
        else
            phase_cnt <= phase_cnt + 1'b1;
    end

    // ── Tile index management ───────────────────────────────────────────
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mt <= '0;
            nt <= '0;
            kt <= '0;
        end else if (state == S_IDLE && start) begin
            mt <= '0;
            nt <= '0;
            kt <= '0;
        end else if (state == S_WRITEBACK && next_state != S_WRITEBACK) begin
            mt <= next_mt;
            nt <= next_nt;
            kt <= next_kt;
        end
    end

    // ── Next-state logic ────────────────────────────────────────────────
    always_comb begin
        next_state = state;
        case (state)
            S_IDLE: begin
                if (start)
                    next_state = S_LOAD_WEIGHTS;
            end

            S_LOAD_WEIGHTS: begin
                if (phase_cnt == 16'(ROWS - 1))
                    next_state = S_FEED;
            end

            S_FEED: begin
                if (phase_cnt == 16'(ROWS - 1))
                    next_state = S_DRAIN;
            end

            S_DRAIN: begin
                if (phase_cnt == 16'(DRAIN_CYCLES - 1))
                    next_state = S_WRITEBACK;
            end

            S_WRITEBACK: begin
                if (phase_cnt == wb_last) begin
                    if (!more_tiles)
                        next_state = S_DONE;
                    else if (next_needs_load)
                        next_state = S_LOAD_WEIGHTS;
                    else
                        next_state = S_FEED;  // reuse weights
                end
            end

            S_DONE: next_state = S_DONE;

            default: next_state = S_IDLE;
        endcase
    end

    // ── Drain register capture ──────────────────────────────────────────
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++)
                    drain_regs[r][c] <= '0;
        end else if (state == S_DRAIN) begin
            for (int j = 0; j < COLS; j++) begin
                automatic int m = int'(phase_cnt) - 1 - j;
                if (m >= 0 && m < ROWS)
                    drain_regs[m][j] <= arr_drain_out[j*ACC_WIDTH +: ACC_WIDTH];
            end
        end
    end

    // ── Output logic ────────────────────────────────────────────────────
    assign done = (state == S_DONE);

    always_comb begin
        // Defaults: everything inactive
        arr_enable      = 1'b0;
        arr_load_weight = 1'b0;
        arr_a_in        = '0;
        arr_b_in        = '0;
        sp_a_addr       = '0;
        sp_b_addr       = '0;
        sp_c_addr       = '0;
        sp_c_we         = 1'b0;
        sp_c_wdata      = '0;

        case (state)
            S_IDLE: begin
                if (start) begin
                    // Pre-read: set SP_B addr for first LOAD_WEIGHTS cycle.
                    // First tile has b_base=0, so addr = ROWS-1.
                    sp_b_addr = ($clog2(SP_B_DEPTH))'(ROWS - 1);
                end
            end

            S_LOAD_WEIGHTS: begin
                arr_enable      = 1'b1;
                arr_load_weight = 1'b1;
                arr_b_in        = sp_b_rdata;  // data from previous cycle's address
                // arr_a_in = 0 flushes skew registers

                // Set NEXT SP_B address (reverse order, one step ahead)
                if (phase_cnt < 16'(ROWS - 1))
                    sp_b_addr = b_base + ($clog2(SP_B_DEPTH))'(ROWS - 2) - ($clog2(SP_B_DEPTH))'(phase_cnt);

                // On last cycle: pre-read SP_A[0] for first FEED cycle
                if (phase_cnt == 16'(ROWS - 1))
                    sp_a_addr = a_base;
            end

            S_FEED: begin
                arr_enable = 1'b1;
                arr_a_in   = sp_a_rdata;  // data from previous cycle's address

                // Set NEXT SP_A address (one step ahead)
                if (phase_cnt < 16'(ROWS - 1))
                    sp_a_addr = a_base + ($clog2(SP_A_DEPTH))'(phase_cnt + 1'b1);
            end

            S_DRAIN: begin
                arr_enable = 1'b1;
                // arr_a_in = 0, arr_b_in = 0: array drains

                // On last cycle: pre-read for WRITEBACK
                if (phase_cnt == 16'(DRAIN_CYCLES - 1) && wb_is_rmw)
                    sp_c_addr = c_base;  // pre-read row 0 for RMW
            end

            S_WRITEBACK: begin
                if (wb_is_rmw) begin
                    // Interleaved RMW: even = write (data ready), odd = read (set addr)
                    if (!phase_cnt[0]) begin
                        // Write cycle: data from pre-read (or previous odd cycle) is in sp_c_rdata
                        sp_c_addr = c_base + ($clog2(SP_C_DEPTH))'(wb_row);
                        sp_c_we   = 1'b1;
                        for (int j = 0; j < COLS; j++)
                            sp_c_wdata[j*ACC_WIDTH +: ACC_WIDTH] =
                                sp_c_rdata[j*ACC_WIDTH +: ACC_WIDTH] +
                                drain_regs[wb_row][j];
                    end else begin
                        // Read cycle: set address for next row
                        sp_c_addr = c_base + ($clog2(SP_C_DEPTH))'(wb_row + 1'b1);
                    end
                end else begin
                    // Direct write: one row per cycle
                    sp_c_addr = c_base + ($clog2(SP_C_DEPTH))'(phase_cnt);
                    sp_c_we   = 1'b1;
                    for (int j = 0; j < COLS; j++)
                        sp_c_wdata[j*ACC_WIDTH +: ACC_WIDTH] = drain_regs[phase_cnt[15:0]][j];
                end

                // On last cycle: pre-read for next tile
                if (phase_cnt == wb_last && more_tiles) begin
                    if (next_needs_load)
                        // Weights change: pre-read SP_B for LOAD_WEIGHTS
                        sp_b_addr = next_b_base + ($clog2(SP_B_DEPTH))'(ROWS - 1);
                    else
                        // Weights reused: pre-read SP_A for FEED
                        sp_a_addr = next_a_base;
                end
            end

            default: ; // IDLE, DONE: all at defaults
        endcase
    end

endmodule
