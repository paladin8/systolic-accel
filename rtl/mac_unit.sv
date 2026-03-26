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
