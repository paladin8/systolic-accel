// NxN weight-stationary systolic array with packed ports.
// Ports use packed bit vectors for Yosys compatibility; internal wiring
// uses unpacked arrays which both Verilator and Yosys handle fine.
module systolic_array #(
    parameter ROWS           = 4,
    parameter COLS           = 4,
    parameter DATA_WIDTH     = 16,
    parameter ACC_WIDTH      = 32,
    parameter PIPELINE_DEPTH = 2
)(
    input  logic                          clk,
    input  logic                          rst_n,
    input  logic                          enable,
    input  logic                          load_weight,
    input  logic [ROWS*DATA_WIDTH-1:0]    a_in,
    input  logic [COLS*DATA_WIDTH-1:0]    b_in,
    output logic [COLS*ACC_WIDTH-1:0]     drain_out
);

    // Internal wires for MAC interconnect
    logic [DATA_WIDTH-1:0] a_wire [0:ROWS-1][0:COLS];
    logic [DATA_WIDTH-1:0] b_wire [0:ROWS][0:COLS-1];
    logic [ACC_WIDTH-1:0]  psum_wire [0:ROWS][0:COLS-1];

    // Row activation skew shift registers (row k gets k registers)
    // Synthesizes to: ROWS*(ROWS-1)/2 registers of DATA_WIDTH bits
    generate
        for (genvar k = 0; k < ROWS; k++) begin : gen_skew
            if (k == 0) begin : no_skew
                // Row 0: direct wiring
                assign a_wire[0][0] = a_in[0*DATA_WIDTH +: DATA_WIDTH];
            end else begin : has_skew
                logic [DATA_WIDTH-1:0] skew_reg [0:k-1];

                always_ff @(posedge clk or negedge rst_n) begin
                    if (!rst_n) begin
                        for (int s = 0; s < k; s++)
                            skew_reg[s] <= '0;
                    end else if (enable) begin
                        skew_reg[0] <= a_in[k*DATA_WIDTH +: DATA_WIDTH];
                        for (int s = 1; s < k; s++)
                            skew_reg[s] <= skew_reg[s-1];
                    end
                end

                assign a_wire[k][0] = skew_reg[k-1];
            end
        end
    endgenerate

    // Top-edge connections
    generate
        for (genvar j = 0; j < COLS; j++) begin : gen_top
            assign psum_wire[0][j] = '0;
            assign b_wire[0][j] = b_in[j*DATA_WIDTH +: DATA_WIDTH];
        end
    endgenerate

    // NxN MAC grid
    // Synthesizes to: ROWS*COLS MAC units
    generate
        for (genvar k = 0; k < ROWS; k++) begin : gen_row
            for (genvar j = 0; j < COLS; j++) begin : gen_col
                mac_unit #(
                    .DATA_WIDTH(DATA_WIDTH),
                    .ACC_WIDTH(ACC_WIDTH),
                    .PIPELINE_DEPTH(PIPELINE_DEPTH)
                ) mac (
                    .clk(clk),
                    .rst_n(rst_n),
                    .enable(enable),
                    .load_weight(load_weight),
                    .a(a_wire[k][j]),
                    .b(b_wire[k][j]),
                    .a_out(a_wire[k][j+1]),
                    .b_out(b_wire[k+1][j]),
                    .psum_in(psum_wire[k][j]),
                    .psum_out(psum_wire[k+1][j])
                );
            end
        end
    endgenerate

    // Bottom-edge drain outputs
    generate
        for (genvar j = 0; j < COLS; j++) begin : gen_drain
            assign drain_out[j*ACC_WIDTH +: ACC_WIDTH] = psum_wire[ROWS][j];
        end
    endgenerate

// ── Formal verification ───────────────────────────────────────────
`ifdef FORMAL
    // -- Common infrastructure --
    logic f_past_valid;
    initial f_past_valid = 1'b0;
    always_ff @(posedge clk)
        f_past_valid <= 1'b1;

    // Reset assumption: start in reset, once released stay released
    initial assume (!rst_n);
    always_ff @(posedge clk)
        if (f_past_valid && $past(rst_n))
            assume (rst_n);

    // ── ARR1: Row 0 direct — no skew delay ────────────────────────
    // Combinational: a_wire[0][0] is directly assigned from a_in
    always_comb
        arr1: assert (a_wire[0][0] == a_in[0 +: DATA_WIDTH]);

    // ── ARR2: Row k delay — activation arrives k cycles late ──────
    // Formal-only delay chains mirror the skew shift registers.
    // f_delay[k] tracks what a_in[k] looked like k cycles ago.
    generate
        for (genvar fk = 1; fk < ROWS; fk++) begin : gen_formal_skew
            // k-deep shift register tracking a_in[fk]
            logic [DATA_WIDTH-1:0] f_delay [0:fk-1];

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    for (int s = 0; s < fk; s++)
                        f_delay[s] <= '0;
                end else if (enable) begin
                    f_delay[0] <= a_in[fk*DATA_WIDTH +: DATA_WIDTH];
                    for (int s = 1; s < fk; s++)
                        f_delay[s] <= f_delay[s-1];
                end
            end

            // After reset, the skew output should match our delay chain
            always_ff @(posedge clk)
                if (f_past_valid && rst_n)
                    assert (a_wire[fk][0] == f_delay[fk-1]); // ARR2
        end
    endgenerate

    // ── ARR3: Skew stall — registers hold when !enable ────────────
    generate
        for (genvar fk = 1; fk < ROWS; fk++) begin : gen_formal_stall
            always_ff @(posedge clk)
                if (f_past_valid && rst_n && $past(rst_n && !enable))
                    assert (a_wire[fk][0] == $past(a_wire[fk][0])); // ARR3
        end
    endgenerate

    // ── ARR4: Top-edge partial sums are zero ──────────────────────
    generate
        for (genvar fj = 0; fj < COLS; fj++) begin : gen_formal_psum
            always_comb
                assert (psum_wire[0][fj] == '0); // ARR4
        end
    endgenerate

    // ── ARR5: Top-edge weight distribution ────────────────────────
    generate
        for (genvar fj = 0; fj < COLS; fj++) begin : gen_formal_btop
            always_comb
                assert (b_wire[0][fj] == b_in[fj*DATA_WIDTH +: DATA_WIDTH]); // ARR5
        end
    endgenerate

    // ── ARR6: Drain output matches bottom psum ────────────────────
    generate
        for (genvar fj = 0; fj < COLS; fj++) begin : gen_formal_drain
            always_comb
                assert (drain_out[fj*ACC_WIDTH +: ACC_WIDTH] == psum_wire[ROWS][fj]); // ARR6
        end
    endgenerate

    // ── ARR7: Cover — last row's skew register propagates a value ─
    always_ff @(posedge clk)
        if (f_past_valid && rst_n)
            arr7: cover (a_wire[ROWS-1][0] != '0);
`endif

endmodule
