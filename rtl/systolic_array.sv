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

endmodule
