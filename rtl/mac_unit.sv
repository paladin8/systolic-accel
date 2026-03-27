module mac_unit #(
    parameter DATA_WIDTH     = 16,
    parameter ACC_WIDTH      = 32,
    parameter PIPELINE_DEPTH = 2   // 1, 2, or 3
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

    // Weight register — loaded during weight phase, stationary during compute
    // Synthesizes to: DATA_WIDTH flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            weight_reg <= '0;
        else if (enable && load_weight)
            weight_reg <= b;
    end

    // Passthrough registers — always stage 1, all depths
    // Synthesizes to: 2 * DATA_WIDTH flip-flops
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_out <= '0;
            b_out <= '0;
        end else if (enable) begin
            a_out <= a;
            b_out <= b;
        end
    end

    generate
        if (PIPELINE_DEPTH == 1) begin : gen_depth1
            // Depth 1: combinational multiply-add, registered at psum_out
            // Synthesizes to: ACC_WIDTH flip-flops (psum_out only)
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    psum_out <= '0;
                else if (enable)
                    psum_out <= psum_in + ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            end

        end else if (PIPELINE_DEPTH == 2) begin : gen_depth2
            // Depth 2: register after multiply, then add
            // Synthesizes to: mult_reg[ACC_WIDTH] + psum_out[ACC_WIDTH] flip-flops
            logic [ACC_WIDTH-1:0] mult_reg;

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    mult_reg <= '0;
                else if (enable)
                    mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    psum_out <= '0;
                else if (enable)
                    psum_out <= psum_in + mult_reg;
            end

        end else if (PIPELINE_DEPTH == 3) begin : gen_depth3
            // Depth 3: register after multiply, register after add, then output
            // Synthesizes to: mult_reg[ACC_WIDTH] + add_reg[ACC_WIDTH] + psum_out[ACC_WIDTH] flip-flops
            logic [ACC_WIDTH-1:0] mult_reg;
            logic [ACC_WIDTH-1:0] add_reg;

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    mult_reg <= '0;
                else if (enable)
                    mult_reg <= ACC_WIDTH'($signed(a)) * ACC_WIDTH'($signed(weight_reg));
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    add_reg <= '0;
                else if (enable)
                    add_reg <= psum_in + mult_reg;
            end

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    psum_out <= '0;
                else if (enable)
                    psum_out <= add_reg;
            end
        end
    endgenerate

endmodule
